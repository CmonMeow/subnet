#pragma once
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cwchar>

// The shell's modal menu must never hold up directory or update traffic.
class DirectoryTray {
    HANDLE thread=nullptr, ready=nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> stopped{false};
    NOTIFYICONDATAW icon={};
    UINT taskbarCreated=0;
    void addIcon() {
        if(Shell_NotifyIconW(NIM_ADD,&icon)){
            icon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&icon);
            KillTimer(icon.hWnd,1);
        }else SetTimer(icon.hWnd,1,1000,nullptr);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
        auto* self=reinterpret_cast<DirectoryTray*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){
            self=static_cast<DirectoryTray*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(self && message==self->taskbarCreated && self->taskbarCreated){
            Shell_NotifyIconW(NIM_DELETE,&self->icon);self->addIcon();return 0;
        }
        switch(message){
        case WM_APP+1:
            if(LOWORD(lParam)==WM_CONTEXTMENU || LOWORD(lParam)==NIN_SELECT || LOWORD(lParam)==NIN_KEYSELECT){
                POINT point;GetCursorPos(&point);
                HMENU menu=CreatePopupMenu();
                if(menu){
                    AppendMenuW(menu,MF_STRING,1,L"Exit directory");SetForegroundWindow(hwnd);
                    const UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);
                    DestroyMenu(menu);
                    if(command==1)PostMessageW(hwnd,WM_CLOSE,0,0);
                    else PostMessageW(hwnd,WM_NULL,0,0);
                }
            }
            break;
        case WM_TIMER: if(self && wParam==1)self->addIcon();break;
        case WM_CLOSE: DestroyWindow(hwnd);break;
        case WM_ENDSESSION: if(wParam)DestroyWindow(hwnd);break;
        case WM_DESTROY:
            if(self){Shell_NotifyIconW(NIM_DELETE,&self->icon);self->stopped=true;self->window=nullptr;}
            PostQuitMessage(0);break;
        default: return DefWindowProcW(hwnd,message,wParam,lParam);
        }
        return 0;
    }
    static DWORD WINAPI run(void* context) {
        auto& self=*static_cast<DirectoryTray*>(context);
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        WNDCLASSW wc={};wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.lpszClassName=L"SubnetDirectoryTray";
        self.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        if(RegisterClassW(&wc)){
            HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"subnet directory",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,&self);
            self.window=hwnd;
            if(hwnd){
                self.icon.hWnd=hwnd;
                self.addIcon();
            }
        }
        SetEvent(self.ready);
        if(self.window){MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}}
        // Also remove the icon if the message loop ends without WM_CLOSE.
        if(HWND hwnd=self.window.load())DestroyWindow(hwnd);
        self.stopped=true;
        UnregisterClassW(wc.lpszClassName,instance);
        return 0;
    }
public:
    bool start(unsigned port,bool updates) {
        icon.cbSize=sizeof(icon);icon.uID=1;icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;icon.uCallbackMessage=WM_APP+1;
        icon.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1));
        swprintf_s(icon.szTip,L"subnet directory | Port %u | Updates %s",port,updates?L"available":L"unavailable");
        ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready)return false;
        thread=CreateThread(nullptr,0,run,this,0,nullptr);if(!thread)return false;
        WaitForSingleObject(ready,INFINITE);
        return window.load()!=nullptr;
    }
    bool exiting() const {return stopped.load();}
    ~DirectoryTray(){
        if(thread){if(HWND hwnd=window.load())PostMessageW(hwnd,WM_CLOSE,0,0);WaitForSingleObject(thread,INFINITE);CloseHandle(thread);}
        if(ready)CloseHandle(ready);
    }
};
