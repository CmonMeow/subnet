#pragma once
#include <windows.h>
#include <cstdint>
#include <sodium.h>

// Ephemeral state passed only through an inherited, unnamed mapping. Never saved.
struct SessionResume {
    static constexpr uint32_t Magic = 0x31525353;
    uint32_t magic=Magic, size=sizeof(SessionResume), mode=0, port=0;
    uint64_t expires=0;
    LONG started=0;
    uint32_t microphoneEnabled=0, updateFailed=0;
    uint32_t reserved[8]={};
    char draft[1025]={};
    unsigned char key[32]={}; // Host: signing seed. Client: expected host public key.
    char address[256]={}, savedAddress[256]={};
    uint32_t lines=0;
    struct Line { SYSTEMTIME time={}; uint32_t kind=0; char text[1153]={}; } history[500];
    ~SessionResume(){sodium_memzero(key,sizeof(key));}
    bool valid() const {
        const auto now=GetTickCount64();
        return magic==Magic && size==sizeof(SessionResume) && (mode==1 || mode==2) &&
            port>0 && port<=65535 && expires>now && expires-now<=180000 &&
            microphoneEnabled<=1 && updateFailed<=1 && draft[1024]==0 && lines<=500 && address[255]==0 && savedAddress[255]==0 &&
            !sodium_is_zero(key,sizeof(key));
    }
};
