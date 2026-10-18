#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <memory>
#include "SessionResume.h"

class UpdateClient {
    std::atomic<bool> stop{false},done{false};
    std::wstring stage;
    bool claimed=false,installing=false,checkSucceeded=false;
    unsigned retrySeconds=5;
    std::string address;
    ULONGLONG nextCheck=0,nextForcedCheck=0;
    uint64_t release=0, skippedRelease=0;
    uint32_t protocol=0;
    std::thread worker;
    bool download(std::string address,bool queued=true);
public:
    explicit UpdateClient(const std::string& address);
    ~UpdateClient();
    void poll();
    bool requestRelease(uint64_t minimum);
    uint64_t readyRelease() const { return done.load() ? release : 0; }
    uint32_t readyProtocol() const { return done.load() ? protocol : 0; }
    bool claimUpdate();
    const char* installBlockReason() const;
    void skipRelease();
    bool install(const SessionResume* resume=nullptr);
    static std::unique_ptr<SessionResume> takeResume();
    static void completeResume(bool success);
    static bool recoveredStartup();
    static int startup(); // -1 means normal launch.
};
