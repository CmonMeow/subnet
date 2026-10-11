#pragma once
#include "Screenshot.h"
#include <algorithm>

class ImageViewer
{
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* viewer = reinterpret_cast<ImageViewer*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        switch (message) {
        case WM_NCCREATE:
            viewer=static_cast<ImageViewer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            viewer->_window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(viewer));
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paint; HDC dc=BeginPaint(window,&paint); RECT rect; GetClientRect(window,&rect);
            FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            const auto image=viewer ? viewer->_image : nullptr;
            if (image && image->valid() && rect.right>0 && rect.bottom>0) {
                const auto& bytes=image->bytes;
                const int width=Screenshot::read32(bytes.data()+4), height=Screenshot::read32(bytes.data()+8);
                const double scale=viewer->_actualSize ? 1.0 : (std::min)(1.0,(std::min)(double(rect.right)/width,double(rect.bottom)/height));
                const int w=(std::max)(1,int(width*scale)),h=(std::max)(1,int(height*scale));
                BITMAPINFO info={}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
                info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                const int limitX=(std::max)(0,(w-int(rect.right)+1)/2),limitY=(std::max)(0,(h-int(rect.bottom)+1)/2);
                viewer->_pan.x=(std::max)(-limitX,(std::min)(limitX,(int)viewer->_pan.x));
                viewer->_pan.y=(std::max)(-limitY,(std::min)(limitY,(int)viewer->_pan.y));
                SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
                StretchDIBits(dc,(rect.right-w)/2+viewer->_pan.x,(rect.bottom-h)/2+viewer->_pan.y,w,h,0,0,width,height,bytes.data()+Screenshot::HeaderBytes,&info,DIB_RGB_COLORS,SRCCOPY);
            }
            EndPaint(window,&paint); break;
        }
        case WM_LBUTTONDBLCLK:
            if(viewer){viewer->_actualSize=!viewer->_actualSize;viewer->_pan={};
                SetWindowTextW(window,viewer->_actualSize ? L"Image - 100% (drag to pan; double-click to fit)" : L"Image - fit (double-click for 100%)");
                InvalidateRect(window,nullptr,FALSE);}
            break;
        case WM_LBUTTONDOWN:
            if(viewer && viewer->_actualSize){viewer->_dragging=true;viewer->_last={(short)LOWORD(lParam),(short)HIWORD(lParam)};SetCapture(window);}
            break;
        case WM_MOUSEMOVE:
            if(viewer && viewer->_dragging){POINT point={(short)LOWORD(lParam),(short)HIWORD(lParam)};
                viewer->_pan.x+=point.x-viewer->_last.x;viewer->_pan.y+=point.y-viewer->_last.y;viewer->_last=point;
                InvalidateRect(window,nullptr,FALSE);}
            break;
        case WM_LBUTTONUP: if(GetCapture()==window)ReleaseCapture(); break;
        case WM_CAPTURECHANGED: if(viewer)viewer->_dragging=false; break;
        case WM_SIZE: InvalidateRect(window,nullptr,FALSE); break;
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: DestroyWindow(window); break;
        case WM_NCDESTROY: SetWindowLongPtrW(window,GWLP_USERDATA,0); if(viewer){viewer->_window=nullptr;viewer->_image.reset();} break;
        default: return DefWindowProcW(window,message,wParam,lParam);
        }
        return 0;
    }
    HWND _window=nullptr;
    bool _actualSize=false,_dragging=false;
    POINT _pan={},_last={};
    std::shared_ptr<const Screenshot> _image;
public:
    ImageViewer() = default;
    ImageViewer(const ImageViewer&) = delete;
    ImageViewer& operator=(const ImageViewer&) = delete;
    ~ImageViewer() { if(IsWindow(_window))DestroyWindow(_window); }
    void clear() { if(IsWindow(_window))DestroyWindow(_window);_window=nullptr;_image.reset(); }
    bool show(std::shared_ptr<const Screenshot> image) {
        if(!image || !image->valid())return false;
        WNDCLASSW wc={};wc.style=CS_DBLCLKS;wc.lpfnWndProc=windowProc;wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"oiImageViewer";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        _image=std::move(image);_actualSize=false;_pan={};_dragging=false;
        if(_window && GetCapture()==_window)ReleaseCapture();
        if(!IsWindow(_window))_window=CreateWindowExW(0,wc.lpszClassName,L"Image",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,960,640,nullptr,nullptr,wc.hInstance,this);
        if(!_window){_image.reset();return false;}
        SetWindowTextW(_window,L"Image - fit (double-click for 100%)");
        MONITORINFO monitor={sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(_window,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT work=monitor.rcWork;
        RECT frame={};AdjustWindowRectEx(&frame,WS_OVERLAPPEDWINDOW,FALSE,0);
        const int borderWidth=frame.right-frame.left,borderHeight=frame.bottom-frame.top;
        const int width=Screenshot::read32(_image->bytes.data()+4),height=Screenshot::read32(_image->bytes.data()+8);
        const int availableWidth=(std::max)(1,int(work.right-work.left)-borderWidth);
        const int availableHeight=(std::max)(1,int(work.bottom-work.top)-borderHeight);
        const double scale=(std::min)(1.0,(std::min)(double(availableWidth)/width,double(availableHeight)/height));
        const int windowWidth=(std::max)(1,int(width*scale))+borderWidth;
        const int windowHeight=(std::max)(1,int(height*scale))+borderHeight;
        if(IsZoomed(_window) || IsIconic(_window))ShowWindow(_window,SW_RESTORE);
        SetWindowPos(_window,nullptr,work.left+(work.right-work.left-windowWidth)/2,
            work.top+(work.bottom-work.top-windowHeight)/2,windowWidth,windowHeight,SWP_NOZORDER|SWP_NOACTIVATE);
        InvalidateRect(_window,nullptr,FALSE);ShowWindow(_window,SW_SHOWNORMAL);SetForegroundWindow(_window);return true;
    }
};
