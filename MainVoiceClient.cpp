

#include "RunClient.h"
#include "ChatCommands.h"
#include "sysdef.h"
#include <string.h>
#include <windows.h>

namespace
{

const int CHAT_CLIENT_WIDTH = 704;
const int CHAT_CLIENT_HEIGHT = 250;

LRESULT CALLBACK ChatWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCLBUTTONDBLCLK:
        if (wParam == HTSYSMENU) return 0;
        return DefWindowProcA(hwnd, message, wParam, lParam);
    case WM_SYSCOMMAND:
        // The native system-menu tracking loop turns a second icon click into
        // SC_CLOSE without delivering WM_NCLBUTTONDBLCLK to this window.
        if ((wParam & 0xfff0) == SC_MOUSEMENU && (wParam & 0x000f) == HTSYSMENU)
        {
            RECT titleBar = {};
            TITLEBARINFO info = {};
            info.cbSize = sizeof(info);
            if (GetTitleBarInfo(hwnd, &info)) titleBar = info.rcTitleBar;
            else GetWindowRect(hwnd, &titleBar);
            const HMENU commands = reinterpret_cast<HMENU>(GetPropA(hwnd, "ChatCommandsMenu"));
            if (!commands) return 0;
            const UINT selected = TrackPopupMenuEx(commands,
                TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                titleBar.left, titleBar.bottom, hwnd, nullptr);
            if (selected) PostMessageA(hwnd, WM_SYSCOMMAND, selected, 0);
            return 0;
        }
        if ((wParam & 0xfff0) == SC_CLOSE && lParam != 0 &&
            SendMessageA(hwnd, WM_NCHITTEST, 0, lParam) == HTSYSMENU)
            return 0;
        if (wParam >= CHAT_MENU_FIRST && wParam < CHAT_MENU_FIRST + sizeof(CHAT_COMMANDS)/sizeof(CHAT_COMMANDS[0]))
        {
            PostMessageA(hwnd, CHAT_MENU_COMMAND, wParam - CHAT_MENU_FIRST, 0);
            return 0;
        }
        return DefWindowProcA(hwnd, message, wParam, lParam);
    case WM_GETMINMAXINFO:
        {
            RECT minimum = {0, 0, CHAT_CLIENT_WIDTH, CHAT_CLIENT_HEIGHT};
            AdjustWindowRectEx(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0);
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {minimum.right-minimum.left, minimum.bottom-minimum.top};
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        App.quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            input.Clear();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return DefWindowProcA(hwnd, message, wParam, lParam);
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        input.Clear();
        if (GetCapture() == hwnd) ReleaseCapture();
        return DefWindowProcA(hwnd, message, wParam, lParam);
    case WM_CAPTURECHANGED:
        input.KeyUp(VK_LBUTTON);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        input.KeyDown((unsigned char)wParam);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        input.KeyUp((unsigned char)wParam);
        return 0;
    case WM_CHAR:
        if (wParam >= 32 && wParam < 127)
        {
            input.TextInput((unsigned char)wParam);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        SetCapture(hwnd);
        input.KeyDown(VK_LBUTTON);
        input.mouse.x = (short)LOWORD(lParam);
        input.mouse.y = (short)HIWORD(lParam);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        input.KeyUp(VK_LBUTTON);
        return 0;
    case WM_MOUSEMOVE:
        input.mouse.x = (short)LOWORD(lParam);
        input.mouse.y = (short)HIWORD(lParam);
        return 0;
    case WM_MOUSEWHEEL:
        input.AddMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) App.size = vec2i(LOWORD(lParam), HIWORD(lParam));
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wParam, lParam);
    }
}

}

int main()
{
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) { char* slash = strrchr(path, '\\'); if (slash) { *slash = 0; SetCurrentDirectoryA(path); } }
    FreeConsole();

    HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = ChatWndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconA(instance, MAKEINTRESOURCEA(IDI_CORE));
    wc.hCursor = LoadCursorA(instance, MAKEINTRESOURCEA(IDC_POINTER_GREEN));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "oiWindow";
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExA(&wc))
    {
        Error("Failed to create chat window class: GetLastError() %d", GetLastError());
        return EXIT_FAILURE;
    }

    RECT rect = { 0, 0, CHAT_CLIENT_WIDTH, CHAT_CLIENT_HEIGHT };
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "Disconnected",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        NULL,
        instance,
        NULL);
    if (!hwnd)
    {
        Error("Failed to create chat window: GetLastError() %d", GetLastError());
        return EXIT_FAILURE;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
    HGLRC context = wglCreateContext(hdc);
    if (!context || !wglMakeCurrent(hdc, context))
    {
        Error("Failed to setup OpenGL chat window: GetLastError() %d", GetLastError());
        return EXIT_FAILURE;
    }

    App.size = vec2i(CHAT_CLIENT_WIDTH, CHAT_CLIENT_HEIGHT);
    RunChatClient(hwnd);

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, instance);
    return EXIT_SUCCESS;
}
