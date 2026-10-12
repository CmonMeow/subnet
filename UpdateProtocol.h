#pragma once
#include <winsock2.h>
#include <windows.h>
#include <sodium.h>
#include <cstdint>
#include <string>
#include <vector>
#include "UpdateKey.h"
#include "UpdateRelease.h"

namespace Updates {
constexpr uint32_t Magic=0x3155494f; // OIU1
constexpr uint64_t MaxBytes=32*1024*1024;
#pragma pack(push,1)
struct Manifest {
    uint32_t magic=Magic;
    uint64_t release=0,bytes=0;
    unsigned char hash[32]={},signature[crypto_sign_BYTES]={};
};
#pragma pack(pop)
inline bool valid(const Manifest& m) {
    return m.magic==Magic && m.release && m.bytes && m.bytes<=MaxBytes &&
        crypto_sign_verify_detached(m.signature,reinterpret_cast<const unsigned char*>(&m),offsetof(Manifest,signature),UpdatePublicKey)==0;
}
inline std::wstring executable() {
    wchar_t path[32768];DWORD n=GetModuleFileNameW(nullptr,path,32768);
    return n && n<32768 ? std::wstring(path,n) : std::wstring();
}
inline std::wstring folder(const std::wstring& path) {return path.substr(0,path.find_last_of(L"\\/"));}
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    File()=default;
    File(const File&)=delete;
    File& operator=(const File&)=delete;
    ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
inline bool readFile(const std::wstring& path,std::vector<unsigned char>& bytes,uint64_t limit=MaxBytes,File* retained=nullptr) {
    File owned;
    HANDLE file=owned.handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info={};LARGE_INTEGER size={};
    bool ok=GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) &&
        GetFileSizeEx(file,&size) && size.QuadPart>0 && uint64_t(size.QuadPart)<=limit;
    if(ok){bytes.resize(size_t(size.QuadPart));DWORD n=0;ok=ReadFile(file,bytes.data(),(DWORD)bytes.size(),&n,nullptr)&&n==bytes.size();}
    if(ok && retained){retained->handle=owned.handle;owned.handle=INVALID_HANDLE_VALUE;}
    if(!ok)bytes.clear();return ok;
}
inline bool loadManifest(const std::wstring& path,Manifest& m) {
    std::vector<unsigned char> bytes;
    if(!readFile(path,bytes,sizeof(m))||bytes.size()!=sizeof(m))return false;
    memcpy(&m,bytes.data(),sizeof(m));return valid(m);
}
inline bool matches(const Manifest& m,const std::vector<unsigned char>& bytes) {
    unsigned char hash[32];
    if(bytes.size()!=m.bytes)return false;
    crypto_hash_sha256(hash,bytes.data(),bytes.size());return sodium_memcmp(hash,m.hash,32)==0;
}
inline bool writeNew(const std::wstring& path,const void* data,DWORD size) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;bool ok=WriteFile(file,data,size,&n,nullptr)&&n==size&&FlushFileBuffers(file);
    CloseHandle(file);if(!ok)DeleteFileW(path.c_str());return ok;
}
}
