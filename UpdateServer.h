#pragma once
#include "UpdateProtocol.h"
#include <array>
#include <list>
#include <algorithm>

// All clients share one verified release. The FIFO stores sockets, never file copies.
class UpdateServer {
    struct Peer {
        SOCKET socket=INVALID_SOCKET;
        size_t sent=0;
        ULONGLONG started=0,progress=0,notice=0;
        bool queuedProtocol=false,announced=false;
    };
    static constexpr size_t MaxWaiting=4096, MaxRequests=128, BytesPerSecond=8*1024*1024;
    static constexpr ULONGLONG QueueTimeout=30*60*1000;
    SOCKET listener=INVALID_SOCKET;
    std::array<Peer,16> active{};
    std::list<Peer> requests,waiting;
    std::vector<unsigned char> release;
    ULONGLONG budgetAt=0,inspectAt=0;
    size_t budget=0,first=0;
    static void close(Peer& p){if(p.socket!=INVALID_SOCKET)closesocket(p.socket);p=Peer{};}
    static bool disconnected(const Peer& p) {
        char extra;const int n=recv(p.socket,&extra,1,MSG_PEEK);
        // There is no valid input after the single request byte.
        return n>=0 || WSAGetLastError()!=WSAEWOULDBLOCK;
    }
public:
    ~UpdateServer(){for(auto& p:active)close(p);for(auto& p:requests)close(p);for(auto& p:waiting)close(p);if(listener!=INVALID_SOCKET)closesocket(listener);}
    bool start(sockaddr_in address,const std::wstring& directory) {
        Updates::Manifest manifest;std::vector<unsigned char> image;
        if(!Updates::loadManifest(directory+L"\\subnet.update",manifest)||!Updates::readFile(directory+L"\\subnet.exe",image)||!Updates::matches(manifest,image))return false;
        release.resize(sizeof(manifest)+image.size());memcpy(release.data(),&manifest,sizeof(manifest));memcpy(release.data()+sizeof(manifest),image.data(),image.size());
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)return false;
        BOOL exclusive=TRUE;setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char*)&exclusive,sizeof(exclusive));
        u_long mode=1;
        if(bind(listener,(sockaddr*)&address,sizeof(address))||listen(listener,SOMAXCONN)||ioctlsocket(listener,FIONBIO,&mode)){closesocket(listener);listener=INVALID_SOCKET;return false;}
        return true;
    }
    void watch(fd_set& reads) const {if(listener!=INVALID_SOCKET)FD_SET(listener,&reads);}
    long pollMicros() const {
        if(!requests.empty() || !waiting.empty())return 10000;
        for(const auto& p:active)if(p.socket!=INVALID_SOCKET)return 10000;
        return 100000;
    }
    void tick() {
        if(listener==INVALID_SOCKET)return;
        const auto now=GetTickCount64();if(now-budgetAt>=1000){budgetAt=now;budget=0;}
        for(unsigned i=0;i<64;++i){
            SOCKET s=accept(listener,nullptr,nullptr);if(s==INVALID_SOCKET)break;
            u_long mode=1;
            if(requests.size()>=MaxRequests || waiting.size()+requests.size()>=MaxWaiting || ioctlsocket(s,FIONBIO,&mode)){closesocket(s);continue;}
            // Small buffers keep thousands of queued connections inexpensive.
            int bytes=4096;setsockopt(s,SOL_SOCKET,SO_RCVBUF,(const char*)&bytes,sizeof(bytes));
            bytes=4096;setsockopt(s,SOL_SOCKET,SO_SNDBUF,(const char*)&bytes,sizeof(bytes));
            requests.push_back(Peer{s,0,now,now,0,false,false});
        }
        for(auto it=requests.begin();it!=requests.end();){
            char request=0;const int n=recv(it->socket,&request,1,0);
            if(n==1 && (request=='U' || request=='Q')){
                it->queuedProtocol=request=='Q';auto ready=it++;waiting.splice(waiting.end(),requests,ready);
            }else if(n>=0 || WSAGetLastError()!=WSAEWOULDBLOCK || now-it->started>=5000){close(*it);it=requests.erase(it);}
            else ++it;
        }
        if(now>=inspectAt){
            inspectAt=now+250;
            for(auto it=waiting.begin();it!=waiting.end();){
                bool failed=now-it->started>=QueueTimeout || disconnected(*it);
                if(!failed && it->queuedProtocol && now>=it->notice){
                    const int n=send(it->socket,"W",1,0);
                    if(n==1){it->notice=now+5000;it->progress=now;}
                    else if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK || now-it->progress>=10000)failed=true;
                }
                if(failed){close(*it);it=waiting.erase(it);}else ++it;
            }
        }
        for(auto& p:active)if(p.socket==INVALID_SOCKET && !waiting.empty()){
            p=waiting.front();waiting.pop_front();p.started=p.progress=now;
            int bytes=65536;setsockopt(p.socket,SOL_SOCKET,SO_SNDBUF,(const char*)&bytes,sizeof(bytes));
        }
        // Rotate the first sender so a busy slot cannot monopolize the bandwidth budget.
        first=(first+1)%active.size();
        for(size_t i=0;i<active.size();++i){
            auto& p=active[(first+i)%active.size()];if(p.socket==INVALID_SOCKET)continue;
            if(now-p.started>=120000 || now-p.progress>=10000 || disconnected(p)){close(p);continue;}
            if(p.queuedProtocol && !p.announced){
                const int n=send(p.socket,"U",1,0);
                if(n==1)p.announced=true;
                else {if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK)close(p);continue;}
            }
            if(budget>=BytesPerSecond)continue;
            const int count=(int)(std::min)({size_t(65536),release.size()-p.sent,BytesPerSecond-budget});
            const int n=send(p.socket,(const char*)release.data()+p.sent,count,0);
            if(n>0){p.progress=now;p.sent+=n;budget+=n;if(p.sent==release.size())close(p);}
            else if(n==0 || WSAGetLastError()!=WSAEWOULDBLOCK)close(p);
        }
    }
};
