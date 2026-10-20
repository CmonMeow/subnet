#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sodium.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace HostDirectory {
constexpr uint32_t Magic = 0x3144494f; // OID1, directory protocol 1.
constexpr unsigned short Port = 778;
constexpr unsigned PageSize = 5, MaxHosts = 2500;
constexpr unsigned MaxHostsPerAddress = 8;
// Keep the directory packet layout compatible. User counts occupy the low 15 bits.
constexpr uint16_t PublicHost=0x8000;
enum class HostFilter : uint16_t { All, Public, Private };
inline bool matchesFilter(uint16_t users,HostFilter filter) {
    return filter==HostFilter::All || ((users&PublicHost)!=0)==(filter==HostFilter::Public);
}
constexpr unsigned long long LeaseMs = 90000;
enum Kind : uint8_t { Browse=1, Challenge, Fetch, List, Register, Probe, Proof, Listed, Retire };
#pragma pack(push,1)
struct Entry {
    uint32_t address;
    uint16_t port, users;
    uint32_t version;
    char name[33];
};
struct Packet {
    uint32_t magic = Magic;
    uint8_t kind = 0, count = 0;
    uint16_t page = 0, pages = 0;
    unsigned char nonce[16] = {}, cookie[16] = {};
    uint16_t gamePort = 0, users = 0;
    uint32_t version = 0;
    char name[33] = {};
    Entry entries[PageSize] = {};
    unsigned char signature[crypto_sign_BYTES] = {};
};
#pragma pack(pop)
// Public verification key only. The private key belongs to the directory operator.
inline const unsigned char* publicKey() {
    static const unsigned char key[crypto_sign_PUBLICKEYBYTES] = {0xf4, 0x1b, 0x50, 0x33, 0x04, 0xce, 0xa6, 0x57, 0x92, 0x8d, 0x09, 0xef, 0xae, 0x6f, 0x32, 0x97, 0xd6, 0xcc, 0x95, 0x44, 0x5d, 0x8a, 0xab, 0x5a, 0xe8, 0x29, 0xa3, 0x0a, 0x14, 0x45, 0xb8, 0xf8};
    return key;
}
inline bool sameEndpoint(const sockaddr_in& a,const sockaddr_in& b) {
    return a.sin_addr.s_addr==b.sin_addr.s_addr && a.sin_port==b.sin_port;
}
inline uint64_t endpointKey(const sockaddr_in& a) { return (uint64_t(a.sin_addr.s_addr)<<16)|a.sin_port; }
inline bool validName(const char* name) {
    size_t n=0;for(;n<33 && name[n];++n)if((unsigned char)name[n]<32||(unsigned char)name[n]>126)return false;
    return n>0 && n<=32 && name[0]!=' ' && name[n-1]!=' ';
}
inline bool decode(const void* data,int length,Packet& p) {
    if(length!=sizeof(p))return false;
    memcpy(&p,data,sizeof(p));return p.magic==Magic && p.kind>=Browse && p.kind<=Retire;
}
inline void sign(Packet& p,const unsigned char* key) {
    crypto_sign_detached(p.signature,nullptr,reinterpret_cast<const unsigned char*>(&p),offsetof(Packet,signature),key);
}
inline bool verified(const Packet& p) {
    return crypto_sign_verify_detached(p.signature,reinterpret_cast<const unsigned char*>(&p),offsetof(Packet,signature),publicKey())==0;
}
inline void send(SOCKET socket,const sockaddr_in& target,const Packet& p) {
    sendto(socket,reinterpret_cast<const char*>(&p),sizeof(p),0,reinterpret_cast<const sockaddr*>(&target),sizeof(target));
}
inline std::string addressText(const Entry& e) {
    in_addr address;address.s_addr=e.address;char text[INET_ADDRSTRLEN]={};
    inet_ntop(AF_INET,&address,text,sizeof(text));return std::string(text)+":"+std::to_string(e.port);
}
}
