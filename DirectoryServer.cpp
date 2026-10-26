#include "HostDirectoryProtocol.h"
#include "UpdateServer.h"
#include "DirectoryTray.h"
#include <windows.h>
#include <map>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdio>

using namespace HostDirectory;
struct Listing { Entry entry; sockaddr_in owner; Packet registration; ULONGLONG expires; };
struct Pending { Packet packet; sockaddr_in owner,game; ULONGLONG expires,lastProbe; };
struct Budget { ULONGLONG start=0;unsigned packets=0; };
static std::atomic<bool> stopping{false};
static std::wstring directoryError;
static int failDirectory(const wchar_t* operation,DWORD code=0) {
    directoryError=operation;
    if(code){
        wchar_t detail[512]={};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,detail,512,nullptr);
        directoryError+=L"\n\nWindows error "+std::to_wstring(code)+L": "+detail;
    }
    fwprintf(stderr,L"%ls\n",directoryError.c_str());
    return 2;
}
static BOOL WINAPI consoleControl(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT){stopping=true;return TRUE;}
    return FALSE;
}
static int runDirectory(int argc,wchar_t** argv,bool console) {
    unsigned port=Port;std::string bindAddress="0.0.0.0";
    for(int i=1;i<argc;++i){
        std::wstring arg=argv[i];
        if(arg==L"--console"){}
        else if(arg==L"--port"&&i+1<argc){wchar_t* end=nullptr;unsigned long n=wcstoul(argv[++i],&end,10);if(!end||*end||n<1||n>65535)return failDirectory(L"Invalid --port. Use a number from 1 to 65535.");port=(unsigned)n;}
        else if(arg==L"--bind"&&i+1<argc){std::wstring value=argv[++i];bindAddress.clear();for(wchar_t c:value){if(c>127)return failDirectory(L"Invalid --bind IPv4 address.");bindAddress.push_back(static_cast<char>(c));}}
        else {return failDirectory(L"Invalid command-line arguments.\nUsage: subnet-directory [--console] [--bind IPv4] [--port 778]");}
    }
    if(sodium_init()<0)return failDirectory(L"Could not initialize directory cryptography.");
    unsigned char key[crypto_sign_SECRETKEYBYTES]={};
    struct ClearKey {unsigned char* key;~ClearKey(){sodium_memzero(key,crypto_sign_SECRETKEYBYTES);}} clearKey{key};
    const auto module=GetModuleHandleW(nullptr);
    const auto resource=FindResourceW(module,MAKEINTRESOURCEW(101),MAKEINTRESOURCEW(10));
    const void* identity=resource?LockResource(LoadResource(module,resource)):nullptr;
    if(!identity || SizeofResource(module,resource)!=sizeof(key)){
        return failDirectory(L"Embedded directory identity is missing or invalid. Rebuild the directory.");
    }
    memcpy(key,identity,sizeof(key));
    unsigned char loadedPublicKey[crypto_sign_PUBLICKEYBYTES];crypto_sign_ed25519_sk_to_pk(loadedPublicKey,key);
    if(sodium_memcmp(loadedPublicKey,publicKey(),sizeof(loadedPublicKey))!=0){return failDirectory(L"Directory key does not match this release. Rebuild the directory with the original identity.");}
    WSADATA wsa;const int startup=WSAStartup(MAKEWORD(2,2),&wsa);if(startup)return failDirectory(L"Could not initialize Windows networking.",startup);
    struct Winsock {~Winsock(){WSACleanup();}} winsock;
    SOCKET socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(socket==INVALID_SOCKET)return failDirectory(L"Could not create the directory UDP socket.",WSAGetLastError());
    struct Socket {SOCKET socket;~Socket(){closesocket(socket);}} closeSocket{socket};
    BOOL exclusive=TRUE;
    if(setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive)))return failDirectory(L"Could not reserve exclusive use of the directory socket.",WSAGetLastError());
    sockaddr_in local={};local.sin_family=AF_INET;local.sin_port=htons((u_short)port);
    if(inet_pton(AF_INET,bindAddress.c_str(),&local.sin_addr)!=1)return failDirectory(L"Invalid --bind IPv4 address.");
    if(bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))==SOCKET_ERROR){
        const DWORD code=WSAGetLastError();
        const std::wstring operation=L"Could not bind directory UDP port "+std::to_wstring(port)+L" on "+std::wstring(bindAddress.begin(),bindAddress.end())+L".";
        return failDirectory(operation.c_str(),code);
    }
    u_long nonblocking=1;
    if(ioctlsocket(socket,FIONBIO,&nonblocking)==SOCKET_ERROR){return failDirectory(L"Could not make the directory socket nonblocking.",WSAGetLastError());}
    UpdateServer updates;
    const bool servingUpdates=updates.start(local,Updates::folder(Updates::executable()));
    if(servingUpdates)puts("Signed updates available on the directory TCP port.");
    else puts("Updates disabled: no matching signed subnet.exe/subnet.update, or TCP bind failed.");
    DirectoryTray tray;
    if(!console && !tray.start(port,servingUpdates))return failDirectory(L"Could not create the directory tray window.",tray.error());
    unsigned char cookieKey[32];randombytes_buf(cookieKey,sizeof(cookieKey));
    auto cookie=[&](const sockaddr_in& from,const Packet& p,ULONGLONG bucket,unsigned char* out){
        unsigned char input[30]={};memcpy(input,&from.sin_addr.s_addr,4);memcpy(input+4,&from.sin_port,2);memcpy(input+6,p.nonce,16);memcpy(input+22,&bucket,8);
        crypto_generichash(out,16,input,sizeof(input),cookieKey,sizeof(cookieKey));
    };
    auto authorized=[&](const sockaddr_in& from,const Packet& p,ULONGLONG now){unsigned char expected[16];for(unsigned age=0;age<2;++age){cookie(from,p,now/30000-age,expected);if(sodium_memcmp(expected,p.cookie,16)==0)return true;}return false;};
    std::map<uint64_t,Listing> hosts;std::map<uint64_t,Pending> pending;
    std::vector<Entry> entries;
    // Fixed, keyed buckets cannot be exhausted by allocating spoofed source addresses.
    // Colliding addresses share a limit; the key changes on each process start.
    std::array<Budget,4096> budgets={};unsigned char rateKey[crypto_shorthash_KEYBYTES];randombytes_buf(rateKey,sizeof(rateKey));
    Budget global;ULONGLONG nextCleanup=0;
    printf("Directory listening on %s UDP %u. Listings expire after 90 seconds. Ctrl+C stops it.\n",bindAddress.c_str(),port);fflush(stdout);
    while(!stopping && !tray.exiting()){
        fd_set reads;FD_ZERO(&reads);FD_SET(socket,&reads);updates.watch(reads);timeval wait={0,updates.pollMicros()};
        if(select(0,&reads,nullptr,nullptr,&wait)==SOCKET_ERROR){return failDirectory(L"Directory network wait failed.",WSAGetLastError());}
        updates.tick();
        auto now=GetTickCount64();
        if(now>=nextCleanup){
            for(auto it=hosts.begin();it!=hosts.end();)if(now>=it->second.expires)it=hosts.erase(it);else ++it;
            for(auto it=pending.begin();it!=pending.end();)if(now>=it->second.expires)it=pending.erase(it);else ++it;
            nextCleanup=now+1000;
        }
        unsigned work=0;
        for(;work<128;++work){
            unsigned char bytes[sizeof(Packet)+1];sockaddr_in from={};int length=sizeof(from);
            int received=recvfrom(socket,reinterpret_cast<char*>(bytes),sizeof(bytes),0,reinterpret_cast<sockaddr*>(&from),&length);
            if(received==SOCKET_ERROR){int error=WSAGetLastError();if(error==WSAEWOULDBLOCK)break;if(error==WSAECONNRESET||error==WSAEMSGSIZE)continue;fprintf(stderr,"Directory receive failed: %d\n",error);break;}
            Packet p;if(!decode(bytes,received,p))continue;
            // Ignore response-only packets before charging request allowances.
            if(p.kind!=Browse&&p.kind!=Fetch&&p.kind!=Register&&p.kind!=Proof&&p.kind!=Retire)continue;
            unsigned char hash[crypto_shorthash_BYTES];
            crypto_shorthash(hash,reinterpret_cast<const unsigned char*>(&from.sin_addr.s_addr),sizeof(from.sin_addr.s_addr),rateKey);
            unsigned bucket=(unsigned(hash[0])|(unsigned(hash[1])<<8))%budgets.size();
            auto& budget=budgets[bucket];if(now-budget.start>=1000)budget={now,0};
            if(budget.packets>=128)continue;++budget.packets;
            if(now-global.start>=1000)global={now,0};if(global.packets>=1000)continue;++global.packets;
            auto reply=[&](Packet response,const sockaddr_in& to){sign(response,key);HostDirectory::send(socket,to,response);};
            if(p.kind==Browse || (p.kind==Register && !authorized(from,p,now))){
                Packet response;response.kind=Challenge;memcpy(response.nonce,p.nonce,16);cookie(from,p,now/30000,response.cookie);reply(response,from);
            }else if(p.kind==Fetch && p.users<=uint16_t(HostFilter::Private) && authorized(from,p,now)){
                entries.clear();for(const auto& h:hosts)if(h.second.expires>now && h.second.entry.version==p.version && matchesFilter(h.second.entry.users,HostFilter(p.users)))entries.push_back(h.second.entry);
                Packet response;response.kind=List;memcpy(response.nonce,p.nonce,16);response.pages=(uint16_t)((std::max)(size_t(1),(entries.size()+PageSize-1)/PageSize));response.page=(std::min)(p.page,uint16_t(response.pages-1));
                size_t first=response.page*PageSize;response.count=(uint8_t)(first<entries.size()?(std::min)(size_t(PageSize),entries.size()-first):0);
                for(unsigned i=0;i<response.count;++i)response.entries[i]=entries[first+i];reply(response,from);
            }else if(p.kind==Register && p.gamePort && p.version && validName(p.name)){
                sockaddr_in game=from;game.sin_port=htons(p.gamePort);auto id=endpointKey(game);
                // Re-acknowledge an already verified registration if its reply was lost.
                auto listed=hosts.find(id);
                if(listed!=hosts.end() && listed->second.expires>now && sameEndpoint(listed->second.owner,from) &&
                   sodium_memcmp(listed->second.registration.nonce,p.nonce,16)==0 && listed->second.entry.users==p.users &&
                   listed->second.entry.version==p.version && memcmp(listed->second.entry.name,p.name,33)==0){
                    Packet response;response.kind=Listed;memcpy(response.nonce,p.nonce,16);reply(response,from);continue;
                }
                if((!hosts.count(id)&&hosts.size()>=MaxHosts)||(!pending.count(id)&&pending.size()>=MaxHosts))continue;
                if(!hosts.count(id)&&!pending.count(id)){
                    unsigned used=0;
                    for(const auto& h:hosts)if(h.second.entry.address==from.sin_addr.s_addr)++used;
                    for(const auto& h:pending)if(h.second.owner.sin_addr.s_addr==from.sin_addr.s_addr&&!hosts.count(h.first))++used;
                    if(used>=MaxHostsPerAddress)continue;
                }
                auto found=pending.find(id);
                // Another registration must not replace a challenge still in flight.
                if(found!=pending.end() && found->second.expires>now &&
                   (!sameEndpoint(found->second.owner,from)||sodium_memcmp(found->second.packet.nonce,p.nonce,16)!=0))continue;
                if(found!=pending.end() && now-found->second.lastProbe<1000)continue;
                if(found!=pending.end() && found->second.expires>now && sameEndpoint(found->second.owner,from) &&
                   sodium_memcmp(found->second.packet.nonce,p.nonce,16)==0 && found->second.packet.users==p.users &&
                   found->second.packet.version==p.version && memcmp(found->second.packet.name,p.name,33)==0){
                    found->second.lastProbe=now;reply(found->second.packet,game);continue;
                }
                Packet probe=p;probe.kind=Probe;memset(probe.entries,0,sizeof(probe.entries));probe.count=0;
                randombytes_buf(probe.cookie,16);pending[id]={probe,from,game,now+8000,now};reply(probe,game);
            }else if(p.kind==Proof){
                auto found=pending.find(endpointKey(from));if(found==pending.end())continue;auto challenge=found->second;
                if(challenge.expires<=now || sodium_memcmp(p.nonce,challenge.packet.nonce,16)||sodium_memcmp(p.cookie,challenge.packet.cookie,16))continue;
                if(!hosts.count(endpointKey(from))&&hosts.size()>=MaxHosts){pending.erase(found);continue;}
                Entry e={from.sin_addr.s_addr,challenge.packet.gamePort,challenge.packet.users,challenge.packet.version,{}};memcpy(e.name,challenge.packet.name,33);
                hosts[endpointKey(from)]={e,challenge.owner,challenge.packet,now+LeaseMs};pending.erase(found);
                Packet response;response.kind=Listed;memcpy(response.nonce,p.nonce,16);reply(response,challenge.owner);
            }else if(p.kind==Retire && p.gamePort){
                sockaddr_in game=from;game.sin_port=htons(p.gamePort);auto found=hosts.find(endpointKey(game));
                if(found!=hosts.end()&&sameEndpoint(found->second.owner,from)&&sodium_memcmp(found->second.registration.nonce,p.nonce,16)==0)hosts.erase(found);
                auto probe=pending.find(endpointKey(game));
                if(probe!=pending.end()&&sameEndpoint(probe->second.owner,from)&&sodium_memcmp(probe->second.packet.nonce,p.nonce,16)==0)pending.erase(probe);
            }
        }
        if(work==128)Sleep(1);
    }
    sodium_memzero(cookieKey,sizeof(cookieKey));sodium_memzero(rateKey,sizeof(rateKey));
    return 0;
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv){failDirectory(L"Could not read command-line arguments.",GetLastError());MessageBoxW(nullptr,directoryError.c_str(),L"subnet directory",MB_OK|MB_ICONERROR);return 2;}
    bool console=false;for(int i=1;i<argc;++i)if(wcscmp(argv[i],L"--console")==0)console=true;
    if(console){
        const bool output=GetFileType(GetStdHandle(STD_OUTPUT_HANDLE))!=FILE_TYPE_UNKNOWN;
        const bool error=GetFileType(GetStdHandle(STD_ERROR_HANDLE))!=FILE_TYPE_UNKNOWN;
        if(!AttachConsole(ATTACH_PARENT_PROCESS))AllocConsole();
        FILE* stream=nullptr;
        if(!output)freopen_s(&stream,"CONOUT$","w",stdout);
        if(!error)freopen_s(&stream,"CONOUT$","w",stderr);
        SetConsoleCtrlHandler(consoleControl,TRUE);
    }
    const int result=runDirectory(argc,argv,console);LocalFree(argv);
    if(result && !console)MessageBoxW(nullptr,directoryError.c_str(),L"subnet directory",MB_OK|MB_ICONERROR);
    return result;
}
