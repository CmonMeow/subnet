#include <winsock2.h>
#include "RunClient.h"
#include "sysdef.h"
#include "NetworkProtocol.h"
#include <map>
#include <deque>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <conio.h>
#include "NetworkRuntime.h"
#include "font.h"
#include "ChatBackground.h"
#include "UpdateClient.h"
static int ChatCursorResource = IDC_POINTER_CYAN;

static void QueueChatRect(float minX,float minY,float maxX,float maxY,float r,float g,float b,float a,bool outline)
{
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(r,g,b,a); glPolygonMode(GL_FRONT_AND_BACK,outline ? GL_LINE : GL_FILL);
    glRectf(minX,minY,maxX,maxY); glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
}
static void QueueChatText(const char* text,float x,float y,float r=.86f,float g=.9f,float b=.88f)
{ drawstring(text,vec3f(x,y,0),vec3f(r,g,b)); }
static bool ChatButtonHovered(vec2i pos)
{
    const int y = App.size.y - input.mouse.y;
    return input.mouse.x >= pos.x && input.mouse.x < pos.x + 128 &&
           y >= pos.y && y < pos.y + 22;
}

static void DrawChatButton(const char* text, vec2i pos, bool on, bool speaking = false, vec3f textColor = vec3f(.92f, .98f, .96f))
{
    QueueChatRect((float)pos.x, (float)pos.y, (float)pos.x + 128.f, (float)pos.y + 22.f,
                  .02f, speaking ? .42f : on ? .10f : .02f, speaking ? .62f : on ? .24f : .02f, .50f, false);
    const bool hovered=ChatButtonHovered(pos);
    if(hovered) ChatCursorResource=IDC_POINTER_GREEN;
    const bool highlighted=on || speaking || hovered;
    const float brightness=speaking || hovered ? 1.f : .8f;
    QueueChatRect((float)pos.x, (float)pos.y, (float)pos.x + 128.f, (float)pos.y + 22.f,
                  highlighted ? .35f*brightness : .35f, highlighted ? .85f*brightness : .35f, highlighted ? brightness : .35f, .95f, true);
    if(speaking)textColor=vec3f(.92f,.98f,1.f);
    else if(highlighted)textColor=vec3f(.35f,.85f,1.f);
    const float textX = (float)pos.x + (128.f - (float)ChatTextWidth(text)) * .5f;
    QueueChatText(text, textX, (float)pos.y + 5.f, textColor.x, textColor.y, textColor.z);
}

#include "ClientSettings.h"
#include "VoiceChat.h"
#include "ChatBox.h"
#include "ScreenShare.h"
#include "HostBrowser.h"
struct ChatParticipantView
{
    int id;
    string label;
    bool local;
    bool speaking;
};
struct ChatStatusView
{
    string status;
    bool online;
    bool connecting;
    vector<ChatParticipantView> people;
};

class ParticipantOrder {
    struct Activity { bool speaking=false; unsigned long long lastSpoke=0; };
    std::map<int,Activity> activity;
    unsigned long long sequence=0;
public:
    void apply(vector<ChatParticipantView>& people) {
        for(auto it=activity.begin();it!=activity.end();){
            if(std::none_of(people.begin(),people.end(),[&](const auto& p){return p.id==it->first;}))it=activity.erase(it);
            else ++it;
        }
        if(people.empty()){sequence=0;return;}
        for(const auto& person:people){
            auto& state=activity[person.id];
            if(person.speaking && !state.speaking)state.lastSpoke=++sequence;
            state.speaking=person.speaking;
        }
        std::stable_sort(people.begin(),people.end(),[&](const auto& a,const auto& b){
            if(a.local!=b.local)return a.local;
            return activity.at(a.id).lastSpoke>activity.at(b.id).lastSpoke;
        });
    }
};

class ParticipantVolume {
    struct Row {int id;float x,y,width;string label;};
    vector<Row> rows;
    size_t offset=0;
    int layoutWidth=0,layoutHeight=0,layoutRows=0;
    int player=-1;float x=0,y=0;Row owner={};bool dragging=false;
    static bool hit(float x,float y,float width,float height) {
        const int mouseY=App.size.y-input.mouse.y;
        return input.mouse.x>=x&&input.mouse.x<x+width&&mouseY>=y&&mouseY<y+height;
    }
public:
    void beginRows(){rows.clear();}
    size_t first(size_t count){if(offset>=count)offset=0;return offset;}
    void row(int id,float x,float y,float width,const string& label=""){rows.push_back({id,x,y,width,label});}
    bool update(cNetworkRuntime& network,cVoiceChat& voice,PackedClientSettings& settings,bool focused,int nameRows) {
        if(layoutWidth!=App.size.x || layoutHeight!=App.size.y || layoutRows!=nameRows){
            layoutWidth=App.size.x;layoutHeight=App.size.y;layoutRows=nameRows;
            rows.clear();player=-1;dragging=false;
        }
        if(!focused){player=-1;dragging=false;return false;}
        if(player>=0){
            bool present=false;for(const auto& p:network.participants())if(p.first==player)present=true;
            if(!present){player=-1;dragging=false;input.KeyUp(VK_LBUTTON);return true;}
            for(const auto& row:rows)if(row.id==player){owner=row;break;}
        }
        const bool overPopup=player>=0&&hit(x,y,200,44);
        if(!dragging&&!overPopup&&hit(12,(float)App.size.y-56-(nameRows-1)*18,(float)App.size.x-24,22.f+(nameRows-1)*18.f)){
            const int wheel=input.ConsumeMouseWheel();
            if(wheel){if(wheel<0)++offset;else if(offset)--offset;player=-1;return true;}
            for(const auto& r:rows)if(r.id==-2&&hit(r.x,r.y-2,r.width,18)&&input.leftClick()){
                ++offset;player=-1;input.KeyUp(VK_LBUTTON);return true;
            }
        }
        // Keep an open popup anchored while it is being used. Otherwise resolve
        // the current row under the mouse, not the selected user's old position.
        if(!dragging&&!overPopup) {
            player=-1;
            for(const auto& row:rows)if(row.id!=network.localPlayerId()&&hit(row.x,row.y-2,row.width,18)){
                bool exists=false;for(const auto& p:network.participants())if(p.first==row.id)exists=true;
                if(!exists)continue;
                owner=row;player=row.id;x=(std::max)(4.f,(std::min)(row.x,(float)App.size.x-204));y=row.y-46;break;
            }
        }
        if(player<0)return false;
        if(dragging && !(GetAsyncKeyState(VK_LBUTTON)&0x8000)){
            dragging=false;input.KeyUp(VK_LBUTTON);return true;
        }
        if(hit(x,y,200,44)&&input.leftClick()){
            if(hit(x+6,y+2,188,20))dragging=true;
            else {input.KeyUp(VK_LBUTTON);return true;}
        }
        if(dragging){
            const int percent=(int)((input.mouse.x-(x+12))*200/176+.5f);
            voice.setUserVolume(player,percent,network,settings);
            input.KeyUp(VK_LBUTTON);
            return true;
        }
        if(hit(x,y,200,44)){
            const int wheel=input.ConsumeMouseWheel();
            if(wheel)voice.setUserVolume(player,voice.userVolume(player)+(wheel>0?5:-5),network,settings);
            return wheel!=0;
        }
        return false;
    }
    void draw(const cVoiceChat& voice) const {
        if(player<0)return;
        if(hit(x,y,200,44))ChatCursorResource=hit(x+6,y+2,188,20)?IDC_POINTER_GREEN:IDC_POINTER_CYAN;
        const int percent=voice.userVolume(player);
        QueueChatRect(x,y,x+200,y+44,.06f,.07f,.07f,1.f,false);
        QueueChatRect(x,y,x+200,y+44,.35f,.85f,1.f,1.f,true);
        const string value=percent?std::to_string(percent)+"%":"Muted";
        const string title=FitChatText(owner.label.empty()?"Volume":owner.label,180-ChatTextWidth(value)-6)+" "+value;
        QueueChatText(title.c_str(),x+10,y+25,.96f,.96f,.96f);
        QueueChatRect(x+12,y+10,x+188,y+13,.02f,.10f,.24f,1.f,false);
        const float thumb=x+12+176*percent/200.f;
        QueueChatRect(x+12,y+10,thumb,y+13,.35f,.85f,1.f,1.f,false);
        QueueChatRect(thumb-3,y+5,thumb+3,y+18,.65f,.92f,1.f,1.f,false);
    }
};

static ChatStatusView MakeChatStatus(const cNetworkRuntime& network, const cVoiceChat& voice, ParticipantOrder& order)
{
    ChatStatusView view;
    view.online = network.isHost() || network.clientReady();
    view.connecting = network.hasConnection() && !view.online;
    view.status = view.connecting ? "Connecting..." : "Disconnected";
    if(!view.online && !network.statusNotice().empty()) view.status = network.statusNotice();
    for (const auto& person : network.participants())
    {
        bool local = person.first == network.localPlayerId();
        const string suffix = local ? " (you)" : person.first == 0 ? " (host)" : "";
        view.people.push_back({person.first, FitChatText(person.second, 210 - ChatTextWidth(suffix)) + suffix, local, voice.speaking(person.first, network)});
    }
    if (view.online) view.status = std::to_string(view.people.size()) + (view.people.size() == 1 ? " user" : " users");
    order.apply(view.people);
    return view;
}

static void DrawChatStatus(const ChatStatusView& view, ParticipantVolume& volumes, size_t historyOffset = 0, bool showPeople = true, int nameRows = 1, bool hosting = false)
{
    volumes.beginRows();
    const string history = historyOffset ? " | history +" + std::to_string(historyOffset) : "";
    const string status = FitChatText(view.status, App.size.x - (hosting?572:436) - ChatTextWidth(history));
    QueueChatText(status.c_str(), 20.f, (float)App.size.y - 23.f,
        view.online ? .45f : .65f, view.online ? .85f : .70f, view.online ? 1.f : .67f);
    if (!history.empty())
        QueueChatText(history.c_str(), 20.f + ChatTextWidth(status), (float)App.size.y - 23.f, .90f, .77f, .46f);
    if (!showPeople) return;
    float x = 20.f, y = (float)App.size.y - 53.f;
    if (view.people.empty())
    {
        QueueChatText(view.connecting || view.online ? "Waiting for server..." : "Use /connect or click HOST", x, y, .43f, .51f, .48f);
        return;
    }
    int row=0;
    for (size_t i = volumes.first(view.people.size()); i < view.people.size(); ++i)
    {
        const auto& person = view.people[i];
        string label = FitChatText(person.label, 210);
        const int width = ChatTextWidth(label) + 24;
        if(row+1<nameRows && x+width>App.size.x-20){x=20.f;y-=18.f;++row;}
        const int reserve = row+1==nameRows && i + 1 < view.people.size() ? 105 : 0;
        if (x + width + reserve > App.size.x - 20)
        {
            string more = "+" + std::to_string(view.people.size() - i) + " more";
            volumes.row(-2,x,y,(float)ChatTextWidth(more));
            QueueChatText(more.c_str(), x, y, .53f, .61f, .58f);
            break;
        }
        if(!person.local)volumes.row(person.id,x,y,(float)width,label);
        QueueChatText(person.speaking ? "*" : "-", x, y, person.speaking ? .35f : .38f, person.speaking ? .85f : .46f, person.speaking ? 1.f : .43f);
        QueueChatText(label.c_str(), x + 16.f, y, person.speaking ? .65f : .57f, person.speaking ? .92f : .66f, person.speaking ? 1.f : .62f);
        x += width + 12;
    }
}

struct MessageChimeCooldown {
    bool focused = true;
    ULONGLONG nextChime = 0;
    void focusChanged(bool active) {
        if(focused != active) { focused = active; nextChime = 0; }
    }
    bool ready(ULONGLONG now) const { return !focused && now >= nextChime; }
    void played(ULONGLONG now) { nextChime = now + 10 * 60 * 1000; }
};

void RunChatClient(HWND hWnd)
{
    HDC dc = GetDC(hWnd);
    ChatBackground background;
    const HCURSOR cyanCursor=LoadCursorA(GetModuleHandleA(nullptr),MAKEINTRESOURCEA(IDC_POINTER_CYAN));
    const HCURSOR greenCursor=LoadCursorA(GetModuleHandleA(nullptr),MAKEINTRESOURCEA(IDC_POINTER_GREEN));
    const HCURSOR yellowCursor=LoadCursorA(GetModuleHandleA(nullptr),MAKEINTRESOURCEA(IDC_POINTER_YELLOW));
    const HCURSOR redCursor=LoadCursorA(GetModuleHandleA(nullptr),MAKEINTRESOURCEA(IDC_POINTER_RED));
    DragAcceptFiles(hWnd,TRUE);
    PackedClientSettings settings;
    cNetworkRuntime network(hWnd, &settings);
    auto resume=UpdateClient::takeResume();
    const bool resumeMic=resume && resume->microphoneEnabled;
    const string resumeDraft=resume?resume->draft:"";
    bool skipFailedUpdate=resume && resume->updateFailed;
    if(resume){
        const bool restored=network.restoreSession(*resume);UpdateClient::completeResume(restored);
        if(restored && resume->updateFailed)network.showNotice("Update failed; previous version restored.",true);
        resume.reset();
        if(!restored){App.quit=true;ReleaseDC(hWnd,dc);return;}
    }
    else {
        const bool started=!settings.dedicated() || network.hostOnPort();
        UpdateClient::completeResume(started);
    }
    HostDirectoryClient directory(HostDirectoryClient::configuredAddress(DEFAULT_NETWORK_ADDRESS), APP_PROTOCOL_VERSION);
    UpdateClient updater(HostDirectoryClient::configuredAddress(DEFAULT_NETWORK_ADDRESS));
    HostBrowser browser;
    cChatBox chatBox;chatBox.restoreDraft(resumeDraft);
    cVoiceChat voiceChat;voiceChat.restoreMicLatch(resumeMic);
    MessageChimeCooldown chimeCooldown;
    std::function<void(bool)> appFocusChanged = [&](bool active) {
        chimeCooldown.focusChanged(active);
        if(active) voiceChat.cancelMessageChime();
    };
    SetPropA(hWnd, "ChatAppFocus", &appFocusChanged);
    auto updateChimeFocus = [&] {
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        appFocusChanged(foregroundProcess == GetCurrentProcessId());
    };
    network.messageReceived = [&] {
        updateChimeFocus();

        const auto now = GetTickCount64();
        if(chimeCooldown.ready(now) && voiceChat.messageChime())
            chimeCooldown.played(now);
    };
    network.activityReceived = [&] {
        updateChimeFocus();
        if(!chimeCooldown.focused) voiceChat.messageChime();
    };
    ParticipantVolume userVolumes;
    ParticipantOrder participantOrder;
    ScreenShare screenShare(hWnd,
        [&](const ScreenSignaling::Event& event){ network.sendScreenEvent(event); },
        [&](const string& message,bool error){ network.showNotice(message,error); });
    network.openScreen=[&](int owner,const string& share){ screenShare.watch(owner,share); };
    unsigned __int64 lastTitleUpdate = 0;
    unsigned __int64 previousIncoming = 0, previousOutgoing = 0;
    bool hadTrafficSample = false;
    string previousTitle;
    double nextDraw = 0;
    constexpr double frameIntervalMS = 1000.0 / 60.0;
    bool selectingHotkey = false;
    unsigned char releaseHotkey = 0;
    HMENU commandMenu = CreatePopupMenu();
    if (commandMenu) SetPropA(hWnd, "ChatCommandsMenu", commandMenu);
    int menuHostState = -1;

    bool clientUpdateAttempted=false;
    uint64_t requestedHostRelease=0;
    bool ticking = false;
    auto tick = [&](bool modal) {
        if(ticking || App.quit) return;
        ticking = true;
        updateChimeFocus();
        updater.poll();
        if(skipFailedUpdate && updater.readyRelease()){updater.skipRelease();skipFailedUpdate=false;}
        if(!network.updatePending())requestedHostRelease=0;
        if(!network.isHost() && network.hostUpdateRelease()>requestedHostRelease &&
            updater.requestRelease(network.hostUpdateRelease()))requestedHostRelease=network.hostUpdateRelease();
        // A protocol-changing update must be downloaded before this client reports ready.
        network.updateDownloadReady(network.isHost() || network.hostUpdateProtocol()<=APP_PROTOCOL_VERSION ||
            (updater.readyRelease()>=network.hostUpdateRelease() && updater.readyProtocol()==network.hostUpdateProtocol()));
        network.localScreenBusy(screenShare.busy());
        if(!modal && !network.updatePending()){
            if(network.isHost() && updater.claimUpdate()){
                if(const char* reason=updater.installBlockReason()){
                    updater.skipRelease();network.showNotice(reason,true);
                }else if(!network.beginHostUpdate(updater.readyRelease(),updater.readyProtocol())){
                    updater.skipRelease();network.showNotice("This update cannot restart the room automatically.",true);
                }
            }else if(!network.hasConnection() && updater.claimUpdate()){
                if(updater.install()){App.quit=true;ticking=false;return;}
                updater.skipRelease();network.showNotice("Could not start update.",true);
            }
        }
        if(!modal && network.hostUpdateReady()){
            auto state=network.updateResume();
            if(state){state->microphoneEnabled=voiceChat.micLatched();strncpy_s(state->draft,chatBox.draft().c_str(),_TRUNCATE);}
            if(state && updater.install(state.get())){App.quit=true;ticking=false;return;}
            updater.skipRelease();network.cancelHostUpdate();
        }
        if(!network.waitingForHost())clientUpdateAttempted=false;
        if(!modal && !clientUpdateAttempted && network.waitingForHost() && network.hostUpdateRelease() && updater.readyRelease()>=network.hostUpdateRelease() &&
            updater.readyRelease() && updater.readyProtocol()==network.hostUpdateProtocol()){
            clientUpdateAttempted=true;
            auto state=network.updateResume();
            if(state){state->microphoneEnabled=voiceChat.micLatched();strncpy_s(state->draft,chatBox.draft().c_str(),_TRUNCATE);}
            if(state && updater.install(state.get())){App.quit=true;ticking=false;return;}
        }
        network.update();
        directory.update(network.isHost(),network.hostingPort(),(unsigned)network.participants().size(),settings.hostName,network.publicHost());
        if(!modal) browser.update(network,directory);
        if(!network.isHost() && !network.clientReady()) screenShare.close();
        ScreenSignaling::Event screenEvent;
        while(network.popScreenEvent(screenEvent)) screenShare.receive(screenEvent);
        screenShare.update();
        const unsigned __int64 now = GetTickCount64();
        if (now - lastTitleUpdate >= 1000 || previousTitle.empty())
        {
            unsigned __int64 incoming = 0, outgoing = 0;
            const bool connected = network.trafficTotals(incoming, outgoing);
            char title[192];
            if (connected)
            {
                const double seconds = (now - lastTitleUpdate) / 1000.0;
                const bool validSample = hadTrafficSample && seconds > 0 &&
                    incoming >= previousIncoming && outgoing >= previousOutgoing;
                const double inputRate = validSample ? (incoming - previousIncoming) * 8.0 / seconds / 1000000.0 : 0;
                const double outputRate = validSample ? (outgoing - previousOutgoing) * 8.0 / seconds / 1000000.0 : 0;
                if (network.isHost())
                    snprintf(title, sizeof(title), "Hosting | Avg ping: %d ms | In: %.3f Mbps | Out: %.3f Mbps",
                             network.latencyMS(), inputRate, outputRate);
                else if (network.clientReady())
                    snprintf(title, sizeof(title), "Ping: %d ms | In: %.3f Mbps | Out: %.3f Mbps",
                             network.latencyMS(), inputRate, outputRate);
                else
                    snprintf(title, sizeof(title), "Connecting | In: %.3f Mbps | Out: %.3f Mbps", inputRate, outputRate);
            }
            else
                snprintf(title, sizeof(title), "Disconnected");
            if (previousTitle != title)
            {
                SetWindowTextA(hWnd, title);
                previousTitle = title;
            }
            hadTrafficSample = connected;
            previousIncoming = incoming;
            previousOutgoing = outgoing;
            lastTitleUpdate = now;
        }
        const vec2i micButton(App.size.x - 140, App.size.y - 28);
        const vec2i screenButton(App.size.x - 412, App.size.y - 28);
        const vec2i hotkeyButton(App.size.x - 276, App.size.y - 28);
        const vec2i privacyButton(App.size.x - 548, App.size.y - 28);
        if(!modal && network.isHost() && ChatButtonHovered(privacyButton) && input.leftClick()){input.KeyUp(VK_LBUTTON);network.togglePublicHost();}
        const bool micClicked = !modal && ChatButtonHovered(micButton) && input.leftClick();
        const bool hotkeyClicked = !modal && ChatButtonHovered(hotkeyButton) && input.leftClick();
        const bool screenClicked=!modal &&ChatButtonHovered(screenButton) && input.leftClick();
        if (micClicked || hotkeyClicked || screenClicked) input.KeyUp(VK_LBUTTON);
        if(screenClicked && network.updatePending() && !screenShare.sharing() && !screenShare.watching()){
            network.showNotice("Host update pending.",true);
        }else if(screenClicked) {
            selectingHotkey=false;releaseHotkey=0;input.Clear();
            if(screenShare.watching()) screenShare.stopWatching();
            else if(network.isHost() || network.clientReady()) screenShare.toggle();
            else if(!network.hasConnection()) {chatBox.deactivate();network.hostOnPort();}
        }
        if (hotkeyClicked)
        {
            selectingHotkey = !selectingHotkey;
            input.Clear();
            voiceChat.resetTransmitMode();
        }
        const int nameRows=IsZoomed(hWnd)?2:1;
        chatBox.nameRows(nameRows);
        const bool volumeInput=modal || userVolumes.update(network,voiceChat,settings,GetForegroundWindow()==hWnd,nameRows);
        if (releaseHotkey && !(GetAsyncKeyState(releaseHotkey) & 0x8000)) releaseHotkey = 0;
        if (!selectingHotkey && !releaseHotkey && !volumeInput) chatBox.update(network);
        input.ConsumeMouseWheel();
        const bool talkDown = !modal && !selectingHotkey && !releaseHotkey &&
            (screenShare.focused() ? (GetAsyncKeyState(settings.talkKey)&0x8000)!=0 :
             GetForegroundWindow()==hWnd && input.pressed(settings.talkKey) && !chatBox.active());
        voiceChat.update(network, talkDown, settings, micClicked);
        if (!settings.flushVolumes(now)) network.showNotice("Could not save user volumes.", true);

        // Audio and network run at least every 10 ms; drawing targets 60 Hz.
        if (!IsIconic(hWnd) && now >= nextDraw)
        {
            nextDraw += frameIntervalMS;
            if (nextDraw <= now) nextDraw = now + frameIntervalMS;
            glViewport(0, 0, App.size.x, App.size.y);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, App.size.x, 0, App.size.y, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            ChatCursorResource=!browser.shown(network) && chatBox.linkHovered(network) ? IDC_POINTER_YELLOW : IDC_POINTER_CYAN;
            background.draw(App.size.x, App.size.y,network.publicRoom());
            chatBox.draw(network, GetForegroundWindow() == hWnd, !browser.shown(network));
            DrawChatStatus(MakeChatStatus(network, voiceChat, participantOrder), userVolumes, chatBox.historyOffset(), !browser.shown(network),nameRows,network.isHost());
            browser.draw(network,directory);
            DrawChatButton(voiceChat.micEnabled() ? "MIC ON" : "MIC OFF",
                                        micButton, voiceChat.micEnabled(), voiceChat.transmitting(network));
            DrawChatButton(selectingHotkey ? "PRESS KEY" : settings.talkKeyLabel().c_str(), hotkeyButton, selectingHotkey);
            DrawChatButton(!network.hasConnection() ? "HOST" : screenShare.watching() ? "STOP WATCH" : screenShare.sharing() ? "STOP SHARE" : "SCREEN",screenButton,screenShare.sharing() || screenShare.watching());
            if(network.isHost())DrawChatButton(network.publicHost()?"PUBLIC":"PRIVATE",privacyButton,network.publicHost());
            userVolumes.draw(voiceChat);
            const HCURSOR cursor=network.isHost()&&!directory.listed() ? redCursor :
                ChatCursorResource==IDC_POINTER_GREEN ? greenCursor : ChatCursorResource==IDC_POINTER_YELLOW ? yellowCursor : cyanCursor;
            if(reinterpret_cast<HCURSOR>(GetClassLongPtrA(hWnd,GCLP_HCURSOR))!=cursor)
                SetClassLongPtrA(hWnd,GCLP_HCURSOR,reinterpret_cast<LONG_PTR>(cursor));
            POINT mouse;GetCursorPos(&mouse);
            if(WindowFromPoint(mouse)==hWnd){ScreenToClient(hWnd,&mouse);
                if(mouse.x>=0 && mouse.y>=0 && mouse.x<App.size.x && mouse.y<App.size.y)SetCursor(cursor);}
            SwapBuffers(dc);
        }

        ticking = false;
    };
    std::function<void()> modalTick = [&]{ tick(true); };
    SetPropA(hWnd, "ChatModalTick", &modalTick);

    while (!App.quit)
    {
        if (commandMenu && menuHostState != (int)network.isHost()) {
            menuHostState = (int)network.isHost();
            while (GetMenuItemCount(commandMenu) > 0) DeleteMenu(commandMenu, 0, MF_BYPOSITION);
            for (size_t i = 0; i < sizeof(CHAT_COMMANDS)/sizeof(CHAT_COMMANDS[0]); ++i)
                if (!CHAT_COMMANDS[i].hostOnly || menuHostState)
                    AppendMenuA(commandMenu, MF_STRING, CHAT_MENU_FIRST + i, CHAT_COMMANDS[i].help);
        }
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.hwnd == hWnd && msg.message == CHAT_MENU_COMMAND) {
                if (msg.wParam < sizeof(CHAT_COMMANDS)/sizeof(CHAT_COMMANDS[0])) {
                    const auto& command = CHAT_COMMANDS[msg.wParam];
                    if (!command.hostOnly || network.isHost()) {
                        selectingHotkey = false;
                        releaseHotkey = 0;
                        input.Clear();
                        chatBox.prepareCommand(command.name);
                        SetFocus(hWnd);
                    }
                }
                continue;
            }
            if (msg.hwnd == hWnd && msg.message == WM_DROPFILES)
            {
                HDROP drop = reinterpret_cast<HDROP>(msg.wParam);
                const UINT count = DragQueryFileW(drop,0xffffffff,nullptr,0);
                for (UINT i = 0; i < count && i < 8; ++i)
                {
                    const UINT length = DragQueryFileW(drop,i,nullptr,0);
                    if (!length || length >= 32768) continue;
                    std::vector<wchar_t> path(length+1);
                    if (DragQueryFileW(drop,i,path.data(),length+1)) network.offerFile(path.data());
                }
                if (count > 8) network.showNotice("Drop at most eight files at once.",true);
                DragFinish(drop);
                continue;
            }
            if (msg.hwnd == hWnd && selectingHotkey && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN))
            {
                if (msg.wParam == VK_ESCAPE)
                    selectingHotkey = false;
                else if (msg.wParam >= VK_BACK && msg.wParam < 255)
                {
                    if (!settings.setTalkKey((unsigned char)msg.wParam))
                        network.showNotice("Could not save PTT setting.", true);
                    selectingHotkey = false;
                }
                releaseHotkey = (unsigned char)msg.wParam;
                input.Clear();
                continue;
            }
            if (msg.hwnd == hWnd && releaseHotkey && msg.wParam == releaseHotkey &&
                (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN ||
                 msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP))
            {
                if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) releaseHotkey = 0;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            // Touch taps can arrive as a complete mouse press/release pair.
            // Let the controls observe each edge before draining the next one.
            if (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONDBLCLK ||
                msg.message == WM_LBUTTONUP)
                break;
        }

        tick(false);

        // Wake for the next frame instead of rounding every frame up to 20 ms.
        const double waitMS = IsIconic(hWnd) ? 10.0 : nextDraw - GetTickCount64();
        Sleep(static_cast<DWORD>((std::max)(1.0, (std::min)(10.0, std::ceil(waitMS)))));
    }

    if (!settings.flushVolumes(GetTickCount64(), true)) Error("Could not save user volumes at shutdown");
    RemovePropA(hWnd, "ChatModalTick");
    screenShare.close();
    RemovePropA(hWnd, "ChatCommandsMenu");
    if(commandMenu) DestroyMenu(commandMenu);
    RemovePropA(hWnd, "ChatAppFocus");
    network.messageReceived={};
    network.activityReceived={};
    network.openScreen={};
    DragAcceptFiles(hWnd,FALSE);
    ReleaseDC(hWnd, dc);
}

