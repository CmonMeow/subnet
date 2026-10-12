#include "UpdateProtocol.h"
#include <wincrypt.h>
#include <sddl.h>
#include <cstdio>

int wmain(int argc,wchar_t** argv) {
    if(sodium_init()<0)return 1;
    if(argc==2 && wcscmp(argv[1],L"--init")==0) {
        unsigned char pk[32],sk[64];crypto_sign_keypair(pk,sk);
        DATA_BLOB input={sizeof(sk),sk},encrypted={};
        if(!CryptProtectData(&input,L"oi release signing",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted))return 1;
        CreateDirectoryW(L"private",nullptr);
        PSECURITY_DESCRIPTOR sd=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;OW)",SDDL_REVISION_1,&sd,nullptr))return 1;
        SECURITY_ATTRIBUTES sa={sizeof(sa),sd,FALSE};
        HANDLE f=CreateFileW(L"private\\update.key",GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);LocalFree(sd);
        DWORD n=0;bool ok=f!=INVALID_HANDLE_VALUE && WriteFile(f,encrypted.pbData,encrypted.cbData,&n,nullptr)&&n==encrypted.cbData&&FlushFileBuffers(f);
        if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);LocalFree(encrypted.pbData);sodium_memzero(sk,sizeof(sk));
        if(!ok)return 1;
        FILE* header=nullptr;fopen_s(&header,"UpdateKey.h","wb");if(!header)return 1;
        fprintf(header,"#pragma once\ninline constexpr unsigned char UpdatePublicKey[32]={");
        for(unsigned i=0;i<32;++i)fprintf(header,"%s0x%02x",i?",":"",pk[i]);fprintf(header,"};\n");fclose(header);
        puts("Created account-protected private/update.key and public UpdateKey.h. Recompile publisher before publishing.");return 0;
    }
    if(argc!=2){puts("Usage: PublishUpdate --init | release-number");return 1;}
    wchar_t* end=nullptr;uint64_t version=_wcstoui64(argv[1],&end,10);if(!version||!end||*end)return 1;
    // Keep the exact file locked through resource validation and signing.
    std::vector<unsigned char> protectedKey,image;Updates::File executableLock;
    if(!Updates::readFile(L"bin\\oi.exe",image,Updates::MaxBytes,&executableLock))return 1;
    HMODULE module=LoadLibraryExW(L"bin\\oi.exe",nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!module)return 1;
    HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(202),MAKEINTRESOURCEW(10));
    const auto* embedded=resource?(const uint32_t*)LockResource(LoadResource(module,resource)):nullptr;
    bool correct=embedded && SizeofResource(module,resource)==4 && *embedded==version;
    FreeLibrary(module);if(!correct){puts("Executable release does not match requested manifest version.");return 1;}
    if(!Updates::readFile(L"private\\update.key",protectedKey,4096))return 1;
    DATA_BLOB input={(DWORD)protectedKey.size(),protectedKey.data()},key={};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&key))return 1;
    unsigned char pk[32]={};bool ok=key.cbData==64;
    if(ok){crypto_sign_ed25519_sk_to_pk(pk,key.pbData);ok=sodium_memcmp(pk,UpdatePublicKey,32)==0;}
    Updates::Manifest manifest;manifest.release=version;manifest.bytes=image.size();crypto_hash_sha256(manifest.hash,image.data(),image.size());
    if(ok)crypto_sign_detached(manifest.signature,nullptr,(const unsigned char*)&manifest,offsetof(Updates::Manifest,signature),key.pbData);
    sodium_memzero(key.pbData,key.cbData);LocalFree(key.pbData);if(!ok)return 1;
    DeleteFileW(L"bin\\oi.update.tmp");
    if(!Updates::writeNew(L"bin\\oi.update.tmp",&manifest,sizeof(manifest))||!MoveFileExW(L"bin\\oi.update.tmp",L"bin\\oi.update",MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return 1;
    printf("Published signed release %llu (%llu bytes).\n",manifest.release,manifest.bytes);return 0;
}

