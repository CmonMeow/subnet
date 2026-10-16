#pragma once
#include "Screenshot.h"
#include <algorithm>
#include "FontGlyphs.h"
#include "resource.h"

class ImageViewer
{
    static RECT closeRect(HWND window) {
        RECT rect;GetClientRect(window,&rect);return {(std::max)(0L,rect.right-32),0,rect.right,22};
    }
    static bool closeHit(HWND window, LPARAM point) {
        POINT p={(short)LOWORD(point),(short)HIWORD(point)};const RECT rect=closeRect(window);return PtInRect(&rect,p)!=FALSE;
    }
    static void drawClose(HWND window,HDC dc,bool hover) {
        const RECT rect=closeRect(window);
        BITMAPINFO info={};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=32;
        info.bmiHeader.biHeight=-22;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap)return;
        HDC buffer=CreateCompatibleDC(dc);
        if(!buffer){DeleteObject(bitmap);return;}
        const auto old=SelectObject(buffer,bitmap);
        if(!BitBlt(buffer,0,0,32,22,dc,rect.left,0,SRCCOPY))ZeroMemory(pixels,32*22*4);
        GdiFlush();
        auto* data=static_cast<unsigned char*>(pixels);
        for(int y=0;y<22;++y)for(int x=0;x<32;++x){
            auto* pixel=data+(y*32+x)*4;
            for(int c=0;c<3;++c)pixel[c]=(unsigned char)(pixel[c]/2+3);
            if(x==0 || x==31 || y==0 || y==21){pixel[0]=hover?255:89;pixel[1]=hover?217:89;pixel[2]=89;}
        }
        const auto& glyph=serif['X'-32];
        for(int y=0;y<16;++y)for(int x=0;x<16;++x)if(glyph[(15-y)*2+x/8]&(128>>(x%8))){
            auto* pixel=data+((3+y)*32+8+x)*4;pixel[0]=hover?255:245;pixel[1]=hover?217:250;pixel[2]=hover?89:235;
        }
        BitBlt(dc,rect.left,0,32,22,buffer,0,0,SRCCOPY);
        SelectObject(buffer,old);DeleteDC(buffer);DeleteObject(bitmap);
    }
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
            drawClose(window,dc,viewer && viewer->_closeHovered);
            EndPaint(window,&paint); break;
        }
        case WM_LBUTTONDBLCLK:
            if(closeHit(window,lParam))break;
            if(viewer){viewer->_actualSize=!viewer->_actualSize;viewer->_pan={};
                SetWindowTextW(window,viewer->_actualSize ? L"Image - 100% (drag to pan; double-click to fit)" : L"Image - fit (double-click for 100%)");
                InvalidateRect(window,nullptr,FALSE);}
            break;
        case WM_LBUTTONDOWN:
            if(viewer && closeHit(window,lParam)){viewer->_closePressed=true;SetCapture(window);break;}
            if(viewer && viewer->_actualSize){viewer->_dragging=true;viewer->_last={(short)LOWORD(lParam),(short)HIWORD(lParam)};SetCapture(window);}
            break;
        case WM_MOUSEMOVE:
            if(viewer){const bool hover=closeHit(window,lParam);
                if(hover!=viewer->_closeHovered){viewer->_closeHovered=hover;const RECT rect=closeRect(window);InvalidateRect(window,&rect,FALSE);}
                TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);}
            if(viewer && viewer->_dragging){POINT point={(short)LOWORD(lParam),(short)HIWORD(lParam)};
                viewer->_pan.x+=point.x-viewer->_last.x;viewer->_pan.y+=point.y-viewer->_last.y;viewer->_last=point;
                InvalidateRect(window,nullptr,FALSE);}
            break;
        case WM_MOUSELEAVE:
            if(viewer){viewer->_closeHovered=false;const RECT rect=closeRect(window);InvalidateRect(window,&rect,FALSE);}break;
        case WM_LBUTTONUP: {
            const bool close=viewer && viewer->_closePressed && closeHit(window,lParam);
            if(GetCapture()==window)ReleaseCapture();
            if(close)DestroyWindow(window);break;
        }
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            if(viewer){viewer->_dragging=false;viewer->_closePressed=false;}
            if(GetCapture()==window)ReleaseCapture();break;
        case WM_CAPTURECHANGED: if(viewer){viewer->_dragging=false;viewer->_closePressed=false;} break;
        case WM_SIZE:
            InvalidateRect(window,nullptr,FALSE);break;
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: DestroyWindow(window); break;
        case WM_NCDESTROY: SetWindowLongPtrW(window,GWLP_USERDATA,0); if(viewer){viewer->_window=nullptr;viewer->_image.reset();} break;
        case WM_SETCURSOR:
            if(LOWORD(lParam)==HTCLIENT){POINT point;GetCursorPos(&point);ScreenToClient(window,&point);
                const RECT rect=closeRect(window);SetCursor(LoadCursorA(GetModuleHandleW(nullptr),MAKEINTRESOURCEA(PtInRect(&rect,point)?IDC_POINTER_GREEN:IDC_POINTER_CYAN)));return TRUE;}
            // Fall through.
        default: return DefWindowProcW(window,message,wParam,lParam);
        }
        return 0;
    }
    HWND _window=nullptr;
    bool _actualSize=false,_dragging=false,_closeHovered=false,_closePressed=false;
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
        wc.lpszClassName=L"oiImageViewer";wc.hCursor=LoadCursorA(GetModuleHandleW(nullptr),MAKEINTRESOURCEA(IDC_POINTER_CYAN));
        if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        _image=std::move(image);_actualSize=false;_pan={};_dragging=false;_closeHovered=false;_closePressed=false;
        if(_window && GetCapture()==_window)ReleaseCapture();
        if(!IsWindow(_window))_window=CreateWindowExW(0,wc.lpszClassName,L"Image",WS_POPUP|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,960,640,nullptr,nullptr,wc.hInstance,this);
        if(!_window){_image.reset();return false;}
        SetWindowTextW(_window,L"Image - fit (double-click for 100%)");
        MONITORINFO monitor={sizeof(MONITORINFO)};
        if(!GetMonitorInfoW(MonitorFromWindow(_window,MONITOR_DEFAULTTONEAREST),&monitor))
            monitor.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
        const RECT work=monitor.rcWork;
        const int width=Screenshot::read32(_image->bytes.data()+4),height=Screenshot::read32(_image->bytes.data()+8);
        const int availableWidth=(std::max)(1,int(work.right-work.left));
        const int availableHeight=(std::max)(1,int(work.bottom-work.top));
        const double scale=(std::min)(1.0,(std::min)(double(availableWidth)/width,double(availableHeight)/height));
        const int windowWidth=(std::max)(32,int(width*scale));
        const int windowHeight=(std::max)(32,int(height*scale));
        if(IsZoomed(_window) || IsIconic(_window))ShowWindow(_window,SW_RESTORE);
        SetWindowPos(_window,nullptr,work.left+(work.right-work.left-windowWidth)/2,
            work.top+(work.bottom-work.top-windowHeight)/2,windowWidth,windowHeight,SWP_NOZORDER|SWP_NOACTIVATE);
        InvalidateRect(_window,nullptr,FALSE);ShowWindow(_window,SW_SHOWNORMAL);SetForegroundWindow(_window);return true;
    }
};
