#pragma once
#include "HostDirectoryClient.h"

class HostBrowser {
    bool wasConnected=false,haveEntries=false;
    unsigned long long entriesRevision=0;
    std::vector<HostDirectory::Entry> displayedEntries;
    unsigned displayedPage=0,displayedPages=1;
    static bool hit(int x,int y,int width,int height) {
        int mouseY=App.size.y-input.mouse.y;
        return input.mouse.x>=x&&input.mouse.x<x+width&&mouseY>=y&&mouseY<y+height;
    }
public:
    void show(HostDirectoryClient& directory){displayedEntries.clear();haveEntries=false;directory.refresh();}
    bool shown(const cNetworkRuntime& network) const {return !network.isHost()&&!network.clientReady();}
    void update(cNetworkRuntime& network,HostDirectoryClient& directory) {
        bool connected=network.hasConnection();if(wasConnected&&!connected)show(directory);wasConnected=connected;
        if(!shown(network)||!input.leftClick())return;
        const int top=App.size.y-34;
        for(size_t i=0;i<displayedEntries.size();++i)if(hit(20,top-64-(int)i*18,App.size.x-40,18)){
            std::string address=HostDirectory::addressText(displayedEntries[i]);input.KeyUp(VK_LBUTTON);
            if(!network.hasConnection())network.connectTo(address);return;
        }
        if(hit(20,48,128,22))directory.refresh(displayedPage);
        else if(hit(156,48,128,22)&&displayedPage>0)directory.refresh(displayedPage-1);
        else if(hit(displayedPage>0?292:156,48,128,22)&&displayedPage+1<displayedPages)
            directory.refresh(displayedPage+1);
        if(hit(12,44,App.size.x-24,top-44))input.KeyUp(VK_LBUTTON);
    }
    void draw(const cNetworkRuntime& network,const HostDirectoryClient& directory) {
        if(!shown(network)){displayedEntries.clear();haveEntries=false;return;}
        if(!haveEntries||entriesRevision!=directory.entriesRevision){
            displayedEntries=directory.entries;displayedPage=directory.page;displayedPages=directory.pages;
            entriesRevision=directory.entriesRevision;haveEntries=true;
        }
        const int top=App.size.y-34;
        QueueChatRect(12,44,(float)App.size.x-12,(float)top,.015f,.035f,.03f,1,false);
        QueueChatRect(12,44,(float)App.size.x-12,(float)top,.3f,.45f,.4f,1,true);
        std::string title="Hosts  "+std::to_string(displayedPage+1)+"/"+std::to_string(displayedPages);
        QueueChatText(title.c_str(),24,(float)top-19,.5f,.9f,.75f);
        std::string status=FitChatText(directory.status,App.size.x-48);
        QueueChatText(status.c_str(),24,(float)top-39,.65f,.72f,.69f);
        for(size_t i=0;i<displayedEntries.size();++i){
            const auto& e=displayedEntries[i];int y=top-64-(int)i*18;
            std::string label=FitChatText(e.name,App.size.x-48);
            if(hit(20,y,App.size.x-40,18))QueueChatRect(20,(float)y,(float)App.size.x-20,(float)y+18,.02f,.15f,.11f,1,false);
            QueueChatText(label.c_str(),24,(float)y+2,.4f,.9f,.68f);
        }
        DrawChatButton("REFRESH",vec2i(20,48),false);
        if(displayedPage>0)DrawChatButton("PREVIOUS",vec2i(156,48),false);
        if(displayedPage+1<displayedPages)DrawChatButton("NEXT",vec2i(displayedPage>0?292:156,48),false);
    }
};
