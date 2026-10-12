#pragma once
#include "UpdateProtocol.h"
#include <array>

// TCP validates the return address. Only a fixed request and one immutable release
// are served; no paths or allocation sizes come from the peer.
class UpdateServer {
    struct Peer {SOCKET socket=INVALID_SOCKET;size_t sent=0;ULONGLONG started=0,progress=0;bool accepted=false;};
    SOCKET listener=INVALID_SOCKET;
    std::array<Peer,16> peers{};
    std::vector<unsigned char> release;
    ULONGLONG budgetAt=0;size_t budget=0;
    static void close(Peer& p){if(p.socket!=INVALID_SOCKET)closesocket(p.socket);p=Peer{};}
public:
    ~UpdateServer(){for(auto& p:peers)close(p);if(listener!=INVALID_SOCKET)closesocket(listener);}
    bool start(sockaddr_in address,const std::wstring& directory) {
        Updates::Manifest manifest;std::vector<unsigned char> image;
        if(!Updates::loadManifest(directory+L"\\oi.update",manifest)||!Updates::readFile(directory+L"\\oi.exe",image)||!Updates::matches(manifest,image))return false;
        release.resize(sizeof(manifest)+image.size());memcpy(release.data(),&manifest,sizeof(manifest));memcpy(release.data()+sizeof(manifest),image.data(),image.size());
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)return false;
        BOOL exclusive=TRUE;setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char*)&exclusive,sizeof(exclusive));
        u_long mode=1;
        if(bind(listener,(sockaddr*)&address,sizeof(address))||listen(listener,16)||ioctlsocket(listener,FIONBIO,&mode)){closesocket(listener);listener=INVALID_SOCKET;return false;}
        return true;
    }
    void watch(fd_set& reads) const {if(listener!=INVALID_SOCKET)FD_SET(listener,&reads);}
    long pollMicros() const {
        for(const auto& p:peers)if(p.socket!=INVALID_SOCKET)return 10000;
        return 100000;
    }
    void tick() {
        if(listener==INVALID_SOCKET)return;
        const auto now=GetTickCount64();if(now-budgetAt>=1000){budgetAt=now;budget=0;}
        for(unsigned i=0;i<16;++i){
            SOCKET s=accept(listener,nullptr,nullptr);if(s==INVALID_SOCKET)break;
            bool kept=false;u_long mode=1;
            if(!ioctlsocket(s,FIONBIO,&mode))for(auto& p:peers)if(p.socket==INVALID_SOCKET){p={s,0,now,now,false};kept=true;break;}
            if(!kept)closesocket(s);
        }
        for(auto& p:peers){
            if(p.socket==INVALID_SOCKET)continue;
            if(now-p.started>(p.accepted?120000u:5000u) || now-p.progress>10000){close(p);continue;}
            if(!p.accepted){
                char request=0;int n=recv(p.socket,&request,1,0);
                if(n==1 && request=='U'){p.accepted=true;p.progress=now;}
                else if(n==0 || n>0 || WSAGetLastError()!=WSAEWOULDBLOCK){close(p);continue;}
            }
            if(!p.accepted || budget>=8*1024*1024)continue;
            const int count=(int)(std::min)(size_t(65536),release.size()-p.sent);
            int n=send(p.socket,(const char*)release.data()+p.sent,count,0);
            if(n>0){p.progress=now;p.sent+=n;budget+=n;if(p.sent==release.size())close(p);}
            else if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK)close(p);
        }
    }
};
