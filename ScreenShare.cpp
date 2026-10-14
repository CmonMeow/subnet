#include <winsock2.h>
#include "sysdef.h"
#include "NetworkProtocol.h"
#include "ScreenShare.h"
#include "ScreenRelay.h"
#include <wrl.h>
#include <shlwapi.h>
#include "third_party/webview2/include/WebView2.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
const wchar_t* const ScreenUrl=L"https://subnet/";
constexpr UINT FocusScreenMessage=WM_APP+1;
constexpr UINT ChooseScreenMessage=WM_APP+2;
constexpr UINT FailedScreenMessage=WM_APP+3;
// WebView initialization callbacks need the same DPI context as their window,
// even when the chat window uses Windows' automatic scaling.
struct ScreenDpiScope {
    DPI_AWARENESS_CONTEXT previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~ScreenDpiScope(){if(previous)SetThreadDpiAwarenessContext(previous);}
};
std::wstring wideScreen(const string& value) {
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),nullptr,0);
    std::wstring result(count,0);if(count)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),(int)value.size(),&result[0],count);return result;
}
string narrowScreen(const wchar_t* value) {
    if(!value)return {};int size=(int)wcslen(value);
    int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,size,nullptr,0,nullptr,nullptr);
    string result(count,0);if(count)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,size,&result[0],count,nullptr,nullptr);return result;
}
string screenMessage(const string& kind,int peer=-1,const string& share={},const string& connection={},const string& body={}) {
    return kind+"\n"+std::to_string(peer)+"\n"+share+"\n"+connection+"\n"+body;
}
}

struct ScreenShare::State : std::enable_shared_from_this<ScreenShare::State>
{
    HWND parent=nullptr,window=nullptr;
    HRESULT com=E_FAIL;
    bool ready=false,active=false,viewMode=false,picking=false;
    ULONGLONG notificationUntil=0,notificationNext=0;
    unsigned generation=0;
    string ownShare;
    ScreenSignaling::Event watching{ScreenSignaling::Close};
    Send send; Notice notice;
    ScreenRelay relay{[this](const ScreenSignaling::Event& event){send(event);}};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    std::deque<string> pending;

    void error(const char* text) { notice(text,true); }
    void error(const char* text,HRESULT result) {
        char code[24];sprintf_s(code," (0x%08lX)",(unsigned long)result);notice(string(text)+code,true);
    }
    void hideShareWindow(){
        if(!viewMode&&window){ShowWindow(window,SW_HIDE);SetForegroundWindow(parent);SetFocus(parent);}
    }
    void chooseSource(){
        if(!web||active||!picking)return;
        ScreenDpiScope dpi;
        ShowWindow(window,viewMode?SW_SHOW:SW_SHOWNORMAL);
        controller->put_IsVisible(TRUE);SetForegroundWindow(window);
        controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        const unsigned current=generation;std::weak_ptr<State> weak=shared_from_this();
        HRESULT result=web->CallDevToolsProtocolMethod(L"Runtime.evaluate",
            LR"json({"expression":"setTimeout(()=>document.getElementById('choose').onclick(),0)","userGesture":true})json",
            Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([weak,current](HRESULT result,LPCWSTR)->HRESULT{
                auto s=weak.lock();if(!s||s->generation!=current)return S_OK;
                if(FAILED(result)){s->picking=false;s->hideShareWindow();s->error("Could not open screen selection.",result);}return S_OK;
            }).Get());
        if(FAILED(result)){picking=false;hideShareWindow();error("Could not open screen selection.",result);}
    }
    void hideShareNotification() {
        const auto now=GetTickCount64();
        if(!active||!web||now>=notificationUntil||now<notificationNext)return;
        notificationNext=now+250;
        UINT32 browser=0;if(FAILED(web->get_BrowserProcessId(&browser))||!browser)return;
        // The browser's Hide link minimizes the bar into the taskbar. Only
        // hide this browser's recognized oi bar; leave the picker untouched.
        EnumWindows([](HWND candidate,LPARAM browser)->BOOL {
            DWORD process=0;GetWindowThreadProcessId(candidate,&process);
            if(process!=(DWORD)browser||!IsWindowVisible(candidate))return TRUE;
            wchar_t cls[64],title[128];GetClassNameW(candidate,cls,64);GetWindowTextW(candidate,title,128);
            if(wcscmp(cls,L"Chrome_WidgetWin_1")!=0)return TRUE;
            if(wcscmp(title,L"subnet is sharing a window.")!=0&&
               wcscmp(title,L"subnet is sharing a window and audio.")!=0&&
               wcscmp(title,L"subnet is sharing your screen.")!=0&&
               wcscmp(title,L"subnet is sharing your screen and audio.")!=0)return TRUE;
            ShowWindowAsync(candidate,SW_HIDE);return TRUE;
        },(LPARAM)browser);
    }
    void post(const string& value) {
        if(ready&&web&&value==screenMessage("choose")){PostMessageW(window,ChooseScreenMessage,0,0);return;}
        if(ready&&web) { if(FAILED(web->PostWebMessageAsString(wideScreen(value).c_str()))) error("Could not communicate with the screen viewer."); }
        else if(pending.size()<256) pending.push_back(value);
    }
    void resized() {
        ScreenDpiScope dpi;
        if(controller&&window){RECT bounds;GetClientRect(window,&bounds);controller->put_Bounds(bounds);}
    }
    void stopWatching(){
        if(!viewMode)return;
        viewMode=false;
        if(!active){SendMessageW(window,WM_CLOSE,0,0);return;}
        post(screenMessage("unwatch"));hideShareWindow();
    }
    void shutdown() {
        ++generation;ready=false;viewMode=false;picking=false;pending.clear();
        if(active)send({ScreenSignaling::Stop,-1,ownShare,{},{}});
        if(watching.peer>=0)send({ScreenSignaling::Close,watching.peer,watching.share,watching.connection,{}});
        active=false;ownShare.clear();watching={ScreenSignaling::Close};
        relay.clear();
        if(controller)controller->Close();
        web.Reset();controller.Reset();environment.Reset();
    }
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
        State* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){state=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)state);state->window=window;}
        if(state){
            // Restore browser focus after activating the native window, outside
            // the activation callback to avoid reentering WebView's focus events.
            if(message==WM_SETFOCUS){PostMessageW(window,FocusScreenMessage,0,0);return 0;}
            if(message==FocusScreenMessage){
                if(state->controller && GetForegroundWindow()==window && GetFocus()==window)
                    state->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                return 0;
            }
            if(message==ChooseScreenMessage){state->chooseSource();return 0;}
            if(message==FailedScreenMessage){state->viewMode=false;SendMessageW(window,WM_CLOSE,0,0);return 0;}
            if(message==WM_SIZE){state->resized();return 0;}
            if(message==WM_DPICHANGED){const RECT& bounds=*reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
            if(message==WM_CLOSE){if(state->viewMode&&state->active){state->stopWatching();return 0;}state->shutdown();DestroyWindow(window);return 0;}
            if(message==WM_NCDESTROY){state->shutdown();state->window=nullptr;SetWindowLongPtrW(window,GWLP_USERDATA,0);}
        }
        return DefWindowProcW(window,message,wp,lp);
    }
    void message(const string& text) {
        if(text.size()>25000)return;
        string fields[5];size_t offset=0;
        for(int i=0;i<4;++i){size_t end=text.find('\n',offset);if(end==string::npos)return;fields[i]=text.substr(offset,end-offset);offset=end+1;}
        fields[4]=text.substr(offset);
        char* end=nullptr;long peer=strtol(fields[1].c_str(),&end,10);
        if(fields[1].empty()||!end||*end||peer< -1||peer>INT_MAX)return;
        const auto& kind=fields[0];
        if(kind=="ready") { ready=true;auto queue=std::move(pending);pending.clear();for(const auto& value:queue)post(value);return; }
        if(kind=="pick-finished"){picking=false;hideShareWindow();return;}
        if(kind=="error"){notice(SanitiseChatLine(fields[4]),true);return;}
        if(kind=="video") {
            unsigned lw=0,lh=0,lf=0,rw=0,rh=0,rf=0;char extra=0;
            if(sscanf(fields[4].c_str(),"%u,%u,%u,%u,%u,%u%c",&lw,&lh,&lf,&rw,&rh,&rf,&extra)!=6 ||
                lw>16384 || lh>16384 || rw>16384 || rh>16384 || lf>1000 || rf>1000)return;
            string title="Screen share";
            if(lw&&lh)title+=" | Sharing "+std::to_string(lw)+" x "+std::to_string(lh)+" | "+std::to_string(lf)+" fps";
            if(rw&&rh)title+=" | Watching "+std::to_string(rw)+" x "+std::to_string(rh)+" | "+std::to_string(rf)+" fps";
            if(window)SetWindowTextW(window,wideScreen(title).c_str());
            return;
        }
        if(!ScreenSignaling::validId(fields[2]))return;
        ScreenSignaling::Event event{0,(int)peer,fields[2],fields[3],fields[4]};
        if(kind=="start") { if(active)return;active=true;ownShare=event.share;event.kind=ScreenSignaling::Start;notificationUntil=GetTickCount64()+5000;notificationNext=0;hideShareWindow(); }
        else if(kind=="stop") { if(!active||ownShare!=event.share)return;relay.stop(event.share);active=false;ownShare.clear();event.kind=ScreenSignaling::Stop; }
        else {
            if(!ScreenSignaling::validId(event.connection))return;
            if(kind=="relay-open") {
                int port=relay.open(event.peer,event.share,event.connection);
                post(screenMessage("relay-port",event.peer,event.share,event.connection,std::to_string(port)));
                return;
            }
            if(kind=="relay-close"){relay.close(event.connection);return;}
            if(kind=="watch"){event.kind=ScreenSignaling::Watch;watching=event;}
            else if(kind=="signal")event.kind=ScreenSignaling::Signal;
            else if(kind=="close"){relay.close(event.connection);event.kind=ScreenSignaling::Close;if(watching.connection==event.connection)watching={ScreenSignaling::Close};}
            else return;
        }
        send(event);
    }
    bool open() {
        ScreenDpiScope dpi;
        if(window){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);return true;}
        if(FAILED(com)){error("Screen sharing could not initialize Windows COM.");return false;}
        WNDCLASSW cls={};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"oiScreenShare";
        cls.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&cls);
        window=CreateWindowExW(0,cls.lpszClassName,L"Screen share",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            CW_USEDEFAULT,CW_USEDEFAULT,1000,720,nullptr,nullptr,cls.hInstance,this);
        if(!window){error("Could not open the screen-share window.",HRESULT_FROM_WIN32(GetLastError()));return false;}
        wchar_t folder[32768];DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",folder,32768);
        if(!count||count>=32768){error("Could not locate the screen viewer's data folder.");PostMessageW(window,WM_CLOSE,0,0);return false;}
        std::wstring profile=std::wstring(folder)+L"\\subnet\\ScreenShare";
        const unsigned current=++generation;std::weak_ptr<State> weak=shared_from_this();
        HRESULT result=CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([weak,current](HRESULT result,ICoreWebView2Environment* env)->HRESULT {
                ScreenDpiScope dpi;
                auto s=weak.lock();if(!s||s->generation!=current||!s->window)return S_OK;
                if(FAILED(result)||!env){s->error("Screen sharing requires Microsoft Edge WebView2 Runtime.",result);PostMessageW(s->window,WM_CLOSE,0,0);return S_OK;}
                s->environment=env;
                HRESULT created=env->CreateCoreWebView2Controller(s->window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([weak,current](HRESULT result,ICoreWebView2Controller* control)->HRESULT {
                        ScreenDpiScope dpi;
                        auto s=weak.lock();if(!s||s->generation!=current||!s->window){if(control)control->Close();return S_OK;}
                        if(FAILED(result)||!control){s->error("Could not initialize the screen viewer.",result);PostMessageW(s->window,WM_CLOSE,0,0);return S_OK;}
                        s->controller=control;control->get_CoreWebView2(&s->web);s->resized();
                        if(!s->web){PostMessageW(s->window,WM_CLOSE,0,0);return S_OK;}
                        const auto configured=[&](HRESULT result) {
                            if(SUCCEEDED(result))return true;
                            s->error("Could not secure the screen viewer.",result);
                            PostMessageW(s->window,WM_CLOSE,0,0);return false;
                        };
                        ComPtr<ICoreWebView2Settings> settings;
                        if(!configured(s->web->get_Settings(&settings)))return S_OK;
                        if(!settings){configured(E_POINTER);return S_OK;}
                        if(!configured(settings->put_AreDevToolsEnabled(FALSE)) ||
                           !configured(settings->put_AreDefaultContextMenusEnabled(FALSE)) ||
                           !configured(settings->put_IsStatusBarEnabled(FALSE)))return S_OK;
                        EventRegistrationToken token;
                        if(!configured(s->web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([weak,current](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
                            auto s=weak.lock();if(!s||s->generation!=current)return S_OK;LPWSTR source=nullptr,text=nullptr;
                            args->get_Source(&source);
                            if(source&&wcscmp(source,ScreenUrl)==0&&SUCCEEDED(args->TryGetWebMessageAsString(&text)))s->message(narrowScreen(text));
                            CoTaskMemFree(source);CoTaskMemFree(text);return S_OK;
                        }).Get(),&token)))return S_OK;
                        if(!configured(s->web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([weak,current](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT {
                            auto s=weak.lock();LPWSTR uri=nullptr;args->get_Uri(&uri);
                            // Reloading would discard media without retiring the native share offer.
                            if(!s||s->generation!=current||s->ready||!uri||wcscmp(uri,ScreenUrl)!=0)args->put_Cancel(TRUE);
                            CoTaskMemFree(uri);return S_OK;
                        }).Get(),&token)))return S_OK;
                        if(!configured(s->web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT {args->put_Handled(TRUE);return S_OK;}).Get(),&token)))return S_OK;
                        if(!configured(s->web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* args)->HRESULT {args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);return S_OK;}).Get(),&token)))return S_OK;
                        if(!configured(s->web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([weak,current](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT {
                            auto s=weak.lock();if(s&&s->generation==current){s->error("Screen viewer stopped unexpectedly.");PostMessageW(s->window,FailedScreenMessage,0,0);}return S_OK;
                        }).Get(),&token)))return S_OK;
                        HRESULT filter=s->web->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        if(FAILED(filter)){s->error("Could not configure the local screen page.",filter);PostMessageW(s->window,WM_CLOSE,0,0);return S_OK;}
                        HRESULT resourceHandler=s->web->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([weak,current](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args)->HRESULT {
                            auto s=weak.lock();if(!s||s->generation!=current||!s->environment)return S_OK;
                            ComPtr<ICoreWebView2WebResourceRequest> request;args->get_Request(&request);LPWSTR uri=nullptr;if(request)request->get_Uri(&uri);
                            ComPtr<IStream> body;bool allowed=uri&&wcscmp(uri,ScreenUrl)==0;CoTaskMemFree(uri);
                            if(allowed){HMODULE module=GetModuleHandleW(nullptr);HRSRC resource=FindResourceA(module,MAKEINTRESOURCEA(200),MAKEINTRESOURCEA(10));
                                if(resource){HGLOBAL loaded=LoadResource(module,resource);const BYTE* data=(const BYTE*)LockResource(loaded);if(data)body.Attach(SHCreateMemStream(data,SizeofResource(module,resource)));}}
                            ComPtr<ICoreWebView2WebResourceResponse> response;
                            s->environment->CreateWebResourceResponse(body.Get(),body?200:403,body?L"OK":L"Forbidden",L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store",&response);
                            args->put_Response(response.Get());return S_OK;
                        }).Get(),&token);
                        if(FAILED(resourceHandler)){s->error("Could not serve the local screen page.",resourceHandler);PostMessageW(s->window,WM_CLOSE,0,0);return S_OK;}
                        if(FAILED(s->web->Navigate(ScreenUrl))){s->error("Could not load the screen viewer.");PostMessageW(s->window,WM_CLOSE,0,0);}
                        return S_OK;
                    }).Get());
                if(FAILED(created)){s->error("Could not create the screen viewer.",created);PostMessageW(s->window,WM_CLOSE,0,0);}return S_OK;
            }).Get());
        if(FAILED(result)){error("Screen sharing requires Microsoft Edge WebView2 Runtime.",result);PostMessageW(window,WM_CLOSE,0,0);return false;}
        return true;
    }
};

ScreenShare::ScreenShare(HWND parent,Send send,Notice notice):state(std::make_shared<State>()) {
    state->parent=parent;state->send=send;state->notice=notice;
    state->com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
}
ScreenShare::~ScreenShare(){close();if(SUCCEEDED(state->com))CoUninitialize();}
void ScreenShare::close(){state->viewMode=false;if(state->window)SendMessageW(state->window,WM_CLOSE,0,0);else if(state->controller||state->active)state->shutdown();}
bool ScreenShare::busy() const{return state->picking || state->active || state->watching.kind!=ScreenSignaling::Close;}
bool ScreenShare::sharing() const{return state->active;}
bool ScreenShare::focused() const{return state->window && GetForegroundWindow()==state->window;}
void ScreenShare::toggle(){
    if(state->active){state->post(screenMessage("stop-local"));return;}
    if(state->picking)return;
    state->picking=true;
    if(state->open())state->post(screenMessage("choose"));else state->picking=false;
}
bool ScreenShare::watching() const{return state->viewMode;}
void ScreenShare::stopWatching(){state->stopWatching();}
void ScreenShare::watch(int owner,const string& share){
    if(share==state->ownShare||!state->open())return;
    state->viewMode=true;ShowWindow(state->window,SW_MAXIMIZE);
    state->post(screenMessage("view",owner,share));
}
void ScreenShare::receive(const ScreenSignaling::Event& event){
    if(event.kind==ScreenSignaling::Media){state->relay.receive(event);return;}
    if(event.kind==ScreenSignaling::Close)state->relay.close(event.connection);
    if(event.kind==ScreenSignaling::Stop)state->relay.stop(event.share);
    if(event.kind==ScreenSignaling::Start)return;
    if(!state->window){if(event.kind==ScreenSignaling::Watch)state->send({ScreenSignaling::Close,event.peer,event.share,event.connection,{}});return;}
    const char* kinds[]={"start","stop","watch","signal","close"};
    if(event.kind<0||event.kind>4)return;
    if(event.kind==ScreenSignaling::Close && state->watching.connection==event.connection)state->watching={ScreenSignaling::Close};
    if(event.kind==ScreenSignaling::Stop && state->watching.share==event.share)state->watching={ScreenSignaling::Close};
    if(event.kind==ScreenSignaling::Stop && state->ownShare==event.share){state->active=false;state->ownShare.clear();}
    state->post(screenMessage(kinds[event.kind],event.peer,event.share,event.connection,event.payload));
}
void ScreenShare::update(){state->relay.update();state->hideShareNotification();}
