#include "HostDirectoryProtocol.h"
#include "UpdateServer.h"
#include <windows.h>
#include <sddl.h>
#include <map>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdio>

using namespace HostDirectory;
static bool identity(const std::wstring& path,unsigned char* key,bool create) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD n=0;LARGE_INTEGER size={};bool ok=GetFileSizeEx(file,&size)&&size.QuadPart==crypto_sign_SECRETKEYBYTES&&ReadFile(file,key,crypto_sign_SECRETKEYBYTES,&n,nullptr)&&n==crypto_sign_SECRETKEYBYTES;CloseHandle(file);return ok;}
    if(!create || GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    unsigned char publicBytes[crypto_sign_PUBLICKEYBYTES];crypto_sign_keypair(publicBytes,key);
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;OW)",SDDL_REVISION_1,&descriptor,nullptr))return false;
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
    file=CreateFileW(path.c_str(),GENERIC_WRITE,0,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);LocalFree(descriptor);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;bool ok=WriteFile(file,key,crypto_sign_SECRETKEYBYTES,&n,nullptr)&&n==crypto_sign_SECRETKEYBYTES;CloseHandle(file);return ok;
}
struct Listing { Entry entry; sockaddr_in owner; Packet registration; ULONGLONG expires; };
struct Pending { Packet packet; sockaddr_in owner,game; ULONGLONG expires,lastProbe; };
struct Budget { ULONGLONG start=0;unsigned packets=0; };
int wmain(int argc,wchar_t** argv) {
    bool init=false;unsigned port=Port;std::wstring keyPath;std::string bindAddress="0.0.0.0";
    for(int i=1;i<argc;++i){
        std::wstring arg=argv[i];
        if(arg==L"--init")init=true;
        else if(arg==L"--key"&&i+1<argc)keyPath=argv[++i];
        else if(arg==L"--port"&&i+1<argc){wchar_t* end=nullptr;unsigned long n=wcstoul(argv[++i],&end,10);if(!end||*end||n<1||n>65535)return 2;port=(unsigned)n;}
        else if(arg==L"--bind"&&i+1<argc){std::wstring value=argv[++i];bindAddress.clear();for(wchar_t c:value){if(c>127)return 2;bindAddress.push_back(static_cast<char>(c));}}
        else {fwprintf(stderr,L"Usage: oi-directory [--init] [--key path] [--bind IPv4] [--port 778]\n");return 2;}
    }
    if(keyPath.empty()) {
        wchar_t executable[32768];DWORD length=GetModuleFileNameW(nullptr,executable,32768);
        if(!length||length>=32768)return 2;
        std::wstring folder(executable,length);auto slash=folder.find_last_of(L"\\/");if(slash==std::wstring::npos)return 2;
        folder.resize(slash);folder+=L"\\..\\private";
        if(init&&!CreateDirectoryW(folder.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return 2;
        keyPath=folder+L"\\directory.key";
    }
    if(sodium_init()<0)return 2;
    unsigned char key[crypto_sign_SECRETKEYBYTES]={};
    if(!identity(keyPath,key,init)){fwprintf(stderr,L"Cannot load directory identity: %ls. Use --init once to create it.\n",keyPath.c_str());return 2;}
    if(init){unsigned char pk[crypto_sign_PUBLICKEYBYTES];crypto_sign_ed25519_sk_to_pk(pk,key);char hex[65];sodium_bin2hex(hex,sizeof(hex),pk,sizeof(pk));printf("Public key: %s\n",hex);fwprintf(stdout,L"Private identity: %ls\n",keyPath.c_str());sodium_memzero(key,sizeof(key));return 0;}
    unsigned char loadedPublicKey[crypto_sign_PUBLICKEYBYTES];crypto_sign_ed25519_sk_to_pk(loadedPublicKey,key);
    if(sodium_memcmp(loadedPublicKey,publicKey(),sizeof(loadedPublicKey))!=0){fprintf(stderr,"Directory key does not match this release. Restore the original identity key or rebuild with the new public key.\n");sodium_memzero(key,sizeof(key));return 2;}
    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
    SOCKET socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(socket==INVALID_SOCKET)return 2;
    sockaddr_in local={};local.sin_family=AF_INET;local.sin_port=htons((u_short)port);
    if(inet_pton(AF_INET,bindAddress.c_str(),&local.sin_addr)!=1 || bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))==SOCKET_ERROR){fprintf(stderr,"Cannot bind directory socket: %d\n",WSAGetLastError());closesocket(socket);WSACleanup();return 2;}
    u_long nonblocking=1;
    if(ioctlsocket(socket,FIONBIO,&nonblocking)==SOCKET_ERROR){fprintf(stderr,"Cannot make directory socket nonblocking: %d\n",WSAGetLastError());closesocket(socket);WSACleanup();return 2;}
    UpdateServer updates;
    if(updates.start(local,Updates::folder(Updates::executable())))puts("Signed updates available on the directory TCP port.");
    else puts("Updates disabled: no matching signed oi.exe/oi.update, or TCP bind failed.");
    unsigned char cookieKey[32];randombytes_buf(cookieKey,sizeof(cookieKey));
    auto cookie=[&](const sockaddr_in& from,const Packet& p,ULONGLONG bucket,unsigned char* out){
        unsigned char input[30]={};memcpy(input,&from.sin_addr.s_addr,4);memcpy(input+4,&from.sin_port,2);memcpy(input+6,p.nonce,16);memcpy(input+22,&bucket,8);
        crypto_generichash(out,16,input,sizeof(input),cookieKey,sizeof(cookieKey));
    };
    auto authorized=[&](const sockaddr_in& from,const Packet& p,ULONGLONG now){unsigned char expected[16];for(unsigned age=0;age<2;++age){cookie(from,p,now/30000-age,expected);if(sodium_memcmp(expected,p.cookie,16)==0)return true;}return false;};
    std::map<uint64_t,Listing> hosts;std::map<uint64_t,Pending> pending;
    // Fixed, keyed buckets cannot be exhausted by allocating spoofed source addresses.
    // Colliding addresses share a limit; the key changes on each process start.
    std::array<Budget,4096> budgets={};unsigned char rateKey[crypto_shorthash_KEYBYTES];randombytes_buf(rateKey,sizeof(rateKey));
    Budget global;ULONGLONG nextCleanup=0;
    printf("Directory listening on %s UDP %u. Listings expire after 90 seconds. Ctrl+C stops it.\n",bindAddress.c_str(),port);fflush(stdout);
    for(;;){
        fd_set reads;FD_ZERO(&reads);FD_SET(socket,&reads);updates.watch(reads);timeval wait={0,updates.pollMicros()};select(0,&reads,nullptr,nullptr,&wait);
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
            }else if(p.kind==Fetch && authorized(from,p,now)){
                std::vector<Entry> entries;for(const auto& h:hosts)if(h.second.expires>now && h.second.entry.version==p.version)entries.push_back(h.second.entry);
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
}
