#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

class UpdateClient {
    std::atomic<bool> stop{false},done{false};
    std::wstring stage;
    bool prompted=false,installing=false;
    std::thread worker;
    void download(std::string address);
public:
    explicit UpdateClient(const std::string& address);
    ~UpdateClient();
    bool prompt(HWND window); // True only after the replacement helper was started.
    static int startup(); // -1 means normal launch.
};
