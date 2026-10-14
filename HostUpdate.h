#pragma once
#include "SessionResume.h"
#include <map>
#include <string>

// Runs on the application thread. Wire messages are accepted only after AEAD verification.
struct HostUpdate {
    enum Kind { Capability=1, Prepare, Ready, Busy, Commit, Committed, Cancel, StatefulCapability, StatefulPrepare };
    enum Phase { Idle, Draining, Committing, Reconnecting };
    struct Peer { bool capable=false, ready=false, committed=false, prepared=false; };
    Phase phase=Idle;
    std::map<int,Peer> peers;
    std::string id, address, savedAddress;
    unsigned char seed[32]={}, publicKey[32]={};
    uint64_t release=0;
    int protocol=0;
    ULONGLONG deadline=0, nextReport=0, nextAttempt=0;
    bool readySent=false, reported=false, stateful=true;
    ~HostUpdate(){sodium_memzero(seed,sizeof(seed));}
    void reset() {
        phase=Idle;id.clear();address.clear();savedAddress.clear();release=0;protocol=0;
        deadline=nextReport=nextAttempt=0;readySent=reported=false;stateful=true;
        sodium_memzero(seed,sizeof(seed));sodium_memzero(publicKey,sizeof(publicKey));
        for(auto& p:peers){p.second.ready=p.second.committed=p.second.prepared=false;}
    }
    bool allReady() const {
        for(const auto& p:peers)if(!p.second.capable || !p.second.ready)return false;
        return true;
    }
    bool allCommitted() const {
        for(const auto& p:peers)if(!p.second.committed)return false;
        return true;
    }
};
