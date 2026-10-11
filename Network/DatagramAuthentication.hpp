#pragma once
#include "DatagramFormat.hpp"
#include <mutex>
#include <cstring>
#include <sodium.h>

// Session keys protect control fields as well as payloads. No persistent identity.
class DatagramAuthentication
{
    std::mutex _mutex;
    unsigned char _receive[32] = {}, _send[32] = {};
    bool _ready = false, _sending = false;
public:
    static constexpr size_t TagBytes = DatagramAuthenticationTagBytes;
    ~DatagramAuthentication()
    {
        sodium_memzero(_receive, sizeof(_receive));
        sodium_memzero(_send, sizeof(_send));
    }
    void setKeys(const unsigned char* receive, const unsigned char* send, bool waitForPeer)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ready) return;
        // Domain separation from the application encryption keys.
        const unsigned char context[] = "oi datagram authentication v1";
        crypto_generichash(_receive, sizeof(_receive), context, sizeof(context), receive, 32);
        crypto_generichash(_send, sizeof(_send), context, sizeof(context), send, 32);
        _ready = true;
        _sending = !waitForPeer;
    }
    size_t sign(unsigned char* packet, size_t bytes)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_sending) return bytes;
        crypto_generichash(packet + bytes, TagBytes, packet, bytes, _send, sizeof(_send));
        return bytes + TagBytes;
    }
    bool verify(const unsigned char* packet, size_t bytes, size_t received)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_ready) return received == bytes;
        if (received != bytes + TagBytes) return false;
        unsigned char tag[TagBytes];
        crypto_generichash(tag, sizeof(tag), packet, bytes, _receive, sizeof(_receive));
        if (sodium_memcmp(tag, packet + bytes, sizeof(tag))) return false;
        // The server's key reply is initially plaintext. Begin signing once the
        // client proves it received that reply; retransmitted replies are safe.
        _sending = true;
        return true;
    }
};
