#pragma once
#include "HostDirectoryProbe.h"
#include "Network/AsyncResolver.h"
#include <vector>
#include <fstream>
#include <algorithm>

class HostDirectoryClient {
    SOCKET socket=INVALID_SOCKET;
    bool winsock=false,resolved=false,resolving=false,hosting=false;
    sockaddr_in directory={},listedDirectory={};
    AsyncResolver resolver;
    std::string hostname,publishedName;
    bool publishedPublic=false;
    HostDirectory::HostFilter filter=HostDirectory::HostFilter::All;
    uint32_t version;
    HostDirectory::Packet query,registration,lastListed;
    bool fetching=false,registering=false,haveListing=false;
    unsigned queryAttempts=0,registrationAttempts=0;
    ULONGLONG nextResolve=0,nextQuery=0,nextRegistration=0,lastList=0,lastPublished=0;
    void cancelRegistration() {
        HostDirectoryProbe::instance().clear();
        if(registering&&socket!=INVALID_SOCKET){auto p=registration;p.kind=HostDirectory::Retire;HostDirectory::send(socket,directory,p);}
        registering=false;
    }
    void retire() {
        cancelRegistration();
        if(haveListing&&socket!=INVALID_SOCKET){auto p=lastListed;p.kind=HostDirectory::Retire;HostDirectory::send(socket,listedDirectory,p);}
        haveListing=false;registering=false;lastPublished=0;
    }
public:
    std::vector<HostDirectory::Entry> entries;
    unsigned page=0,pages=1;
    unsigned long long entriesRevision=0;
    std::string status="Finding hosts...",publishStatus;
    explicit HostDirectoryClient(const std::string& address,uint32_t appVersion):hostname(address),version(appVersion) {
        sodium_init();directory.sin_family=AF_INET;directory.sin_port=htons(HostDirectory::Port);
        auto colon=hostname.find(':');if(colon!=std::string::npos){
            std::string text=hostname.substr(colon+1);char* end=nullptr;unsigned long port=strtoul(text.c_str(),&end,10);
            if(text.empty()||!end||*end||port<1||port>65535){status="Invalid directory address.";return;}
            directory.sin_port=htons((u_short)port);hostname.resize(colon);
        }
        if(hostname.empty()||hostname.size()>253){status="Invalid directory address.";return;}
        WSADATA data;winsock=WSAStartup(MAKEWORD(2,2),&data)==0;if(!winsock){status="Directory unavailable.";return;}
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(socket==INVALID_SOCKET){status="Directory unavailable.";return;}
        u_long nonblocking=1;
        if(ioctlsocket(socket,FIONBIO,&nonblocking)==SOCKET_ERROR){closesocket(socket);socket=INVALID_SOCKET;status="Directory unavailable.";}
    }
    ~HostDirectoryClient(){retire();resolver.cancel();if(socket!=INVALID_SOCKET)closesocket(socket);if(winsock)WSACleanup();}
    HostDirectoryClient(const HostDirectoryClient&)=delete;
    HostDirectoryClient& operator=(const HostDirectoryClient&)=delete;
    static std::string configuredAddress(const char* fallback) {
        std::ifstream file("DirectoryAddress.txt");char text[256]={};
        return file.getline(text,sizeof(text))&&text[0]&&_stricmp(text,"nigger.observer")!=0?text:fallback;
    }
    void refresh(unsigned requestedPage=0) {
        if(socket==INVALID_SOCKET)return;
        if(requestedPage!=page){entries.clear();++entriesRevision;}
        page=requestedPage;fetching=false;nextQuery=0;status="Finding hosts...";
    }
    HostDirectory::HostFilter hostFilter() const {return filter;}
    void cycleFilter() {
        filter=HostDirectory::HostFilter((unsigned(filter)+1)%3);
        entries.clear();++entriesRevision;pages=1;refresh();
    }
    void update(bool isHost,unsigned short port,unsigned users,const std::string& hostName,bool publicHost=false) {
        using namespace HostDirectory;auto now=GetTickCount64();
        if(isHost&&(publishedName!=hostName || publishedPublic!=publicHost)){cancelRegistration();publishedName=hostName;publishedPublic=publicHost;nextRegistration=0;}
        if(hosting&&!isHost)retire();if(hosting!=isHost){hosting=isHost;nextRegistration=0;publishStatus=hosting?"Publishing host...":"";}
        // Expiry must run even while DNS resolution is failing.
        if(lastList&&now-lastList>=LeaseMs){entries.clear();++entriesRevision;lastList=0;page=0;pages=1;status="Directory unavailable. Use /connect or the HOST button.";}
        if(lastPublished&&now-lastPublished>=LeaseMs)publishStatus="Host listing expired. Check UDP forwarding.";
        if(socket==INVALID_SOCKET){if(hosting)publishStatus="Directory unavailable. Direct connections still work.";return;}
        if(!resolved){
            if(!resolving&&now>=nextResolve){resolver.start(hostname);resolving=true;nextResolve=now+30000;}
            if(resolving){int result=resolver.poll(directory);if(result){resolving=false;resolved=result>0;if(!resolved){status="Directory unavailable. Use /connect or the HOST button.";if(hosting)publishStatus="Directory unavailable. Direct connections still work.";}}}
            if(!resolved)return;
        }
        for(unsigned work=0;work<32;++work){
            unsigned char bytes[sizeof(Packet)+1];sockaddr_in from={};int size=sizeof(from);
            int length=recvfrom(socket,reinterpret_cast<char*>(bytes),sizeof(bytes),0,reinterpret_cast<sockaddr*>(&from),&size);
            if(length==SOCKET_ERROR){if(WSAGetLastError()==WSAEWOULDBLOCK)break;continue;}
            Packet p;if(!sameEndpoint(from,directory)||!decode(bytes,length,p))continue;
            const bool expectedQuery=fetching&&sodium_memcmp(p.nonce,query.nonce,16)==0&&(p.kind==Challenge||p.kind==List);
            const bool expectedRegistration=hosting&&registering&&sodium_memcmp(p.nonce,registration.nonce,16)==0&&(p.kind==Challenge||p.kind==Listed);
            if((!expectedQuery&&!expectedRegistration)||!verified(p))continue;
            if(fetching&&sodium_memcmp(p.nonce,query.nonce,16)==0){
                if(p.kind==Challenge&&query.kind==Browse){query.kind=Fetch;memcpy(query.cookie,p.cookie,16);send(socket,directory,query);}
                else if(p.kind==List && p.count<=PageSize && p.pages>=1 && p.pages<=(MaxHosts+PageSize-1)/PageSize && p.page<p.pages){
                    bool valid=true;for(unsigned i=0;i<p.count;++i)if(!validName(p.entries[i].name)||!p.entries[i].port||!p.entries[i].address||p.entries[i].version!=version)valid=false;
                    if(!valid)continue;
                    entries.assign(p.entries,p.entries+p.count);++entriesRevision;page=p.page;pages=p.pages;fetching=false;lastList=now;nextQuery=now+15000;
                    // Older directories paginate all hosts; still honor the selected type locally.
                    entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const Entry& e){return !matchesFilter(e.users,filter);}),entries.end());
                    status=entries.empty()?"No matching hosts. Click HOST to start one.":"Click a host to join.";
                }
            }
            if(hosting&&registering&&sodium_memcmp(p.nonce,registration.nonce,16)==0){
                if(p.kind==Challenge&&sodium_is_zero(registration.cookie,16)){memcpy(registration.cookie,p.cookie,16);send(socket,directory,registration);}
                else if(p.kind==Listed){registering=false;haveListing=true;lastListed=registration;listedDirectory=directory;lastPublished=now;nextRegistration=now+20000;publishStatus="Listed in host browser.";}
            }
        }
        if(now>=nextQuery){
            if(!fetching){query=Packet{};query.kind=Browse;query.version=version;query.page=(uint16_t)page;query.users=uint16_t(filter);randombytes_buf(query.nonce,16);queryAttempts=0;fetching=true;}
            if(queryAttempts++>=4){
                fetching=false;nextQuery=now+15000;
                status=entries.empty()?"Directory unavailable. Use /connect or the HOST button.":"Directory unavailable; showing last known hosts.";
                if(now>=nextResolve){cancelRegistration();resolved=false;nextRegistration=0;return;}
            }
            else {send(socket,directory,query);nextQuery=now+2000;}
        }
        if(hosting&&now>=nextRegistration){
            if(!registering){
                registration=Packet{};registration.kind=Register;registration.gamePort=port;registration.users=(uint16_t)(std::min)(users,32767u)|(publicHost?PublicHost:0);registration.version=version;
                const std::string name=hostName.empty()?"Host":hostName;strncpy_s(registration.name,name.c_str(),32);
                randombytes_buf(registration.nonce,16);registrationAttempts=0;registering=true;HostDirectoryProbe::instance().set(directory,registration);
            }
            if(registrationAttempts++>=4){cancelRegistration();nextRegistration=now+20000;publishStatus="Host not listed. Check directory and UDP forwarding.";}
            else {send(socket,directory,registration);nextRegistration=now+2000;}
        }
    }
};
