#pragma once
#include <windows.h>
constexpr UINT CHAT_MENU_COMMAND = WM_APP + 41;
constexpr UINT CHAT_MENU_FIRST = 0x1000;
void RunChatClient(HWND);
int RunDedicatedServer();
