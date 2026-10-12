#include "UpdateProtocol.h"
#include "UpdateClient.h"
#include "Network/AsyncResolver.h"
#include <shellapi.h>
#include <sddl.h>

namespace {
std::wstring quote(const std::wstring& s){return L"\""+s+L"\"";}
bool launch(const std::wstring& exe,const std::wstring& args) {
    std::wstring command=quote(exe)+L" "+args;STARTUPINFOW si={sizeof(si)};PROCESS_INFORMATION pi={};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,Updates::folder(exe).c_str(),&si,&pi))return false;
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return true;
}
bool stageName(const std::wstring& path) {
    const auto name=path.substr(path.find_last_of(L"\\/")+1);
    if(name.size()!=43 || name.compare(0,11,L".oi-update-")!=0)return false;
    for(size_t i=11;i<name.size();++i)if(!iswxdigit(name[i]))return false;
    return true;
}
void cleanup(const std::wstring& path) {
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(!stageName(path)||attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
    DeleteFileW((path+L"\\oi.ready").c_str());DeleteFileW((path+L"\\oi.exe").c_str());DeleteFileW((path+L"\\oi.update").c_str());RemoveDirectoryW(path.c_str());
}
bool plainFile(const std::wstring& path) {
    const DWORD attr=GetFileAttributesW(path.c_str());
    return attr!=INVALID_FILE_ATTRIBUTES && !(attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
}
UpdateClient::UpdateClient(const std::string& address):worker([this,address]{try {download(address);} catch(const std::bad_alloc&) {} done.store(true);}){}
UpdateClient::~UpdateClient(){stop=true;if(worker.joinable())worker.join();if(!installing&&!stage.empty())cleanup(stage);}
void UpdateClient::download(std::string address) {
    sockaddr_in remote={};remote.sin_family=AF_INET;remote.sin_port=htons(778);
    const auto colon=address.find(':');if(colon!=std::string::npos){
        const auto port=address.substr(colon+1);char* end=nullptr;unsigned long n=strtoul(port.c_str(),&end,10);
        if(port.empty()||*end||n<1||n>65535)return;remote.sin_port=htons((u_short)n);address.resize(colon);
    }
    AsyncResolver resolver;resolver.start(address);int result=0;
    while(!stop && !(result=resolver.poll(remote)))Sleep(20);
    if(stop||result<0)return;
    SOCKET socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(socket==INVALID_SOCKET)return;
    struct Close {SOCKET s;~Close(){closesocket(s);}} close{socket};
    u_long mode=1;if(ioctlsocket(socket,FIONBIO,&mode))return;
    const ULONGLONG started=GetTickCount64();
    auto wait=[&](bool writing,ULONGLONG deadline){
        while(!stop && GetTickCount64()<deadline){fd_set set;FD_ZERO(&set);FD_SET(socket,&set);timeval timeout={0,100000};
            int n=select(0,writing?nullptr:&set,writing?&set:nullptr,nullptr,&timeout);if(n>0)return true;if(n<0)return false;}
        return false;
    };
    if(connect(socket,(sockaddr*)&remote,sizeof(remote))==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)return;
    if(!wait(true,started+10000))return;
    int error=0,size=sizeof(error);if(getsockopt(socket,SOL_SOCKET,SO_ERROR,(char*)&error,&size)||error)return;
    if(send(socket,"U",1,0)!=1)return;
    auto receive=[&](void* data,size_t bytes){
        auto* out=(char*)data;ULONGLONG progress=GetTickCount64();
        while(bytes && !stop){
            if(!wait(false,(std::min)(started+120000,progress+10000)))return false;
            int n=recv(socket,out,(int)(std::min)(bytes,size_t(65536)),0);if(n<=0){if(n<0&&WSAGetLastError()==WSAEWOULDBLOCK)continue;return false;}
            out+=n;bytes-=n;progress=GetTickCount64();
        }
        return bytes==0 && !stop;
    };
    Updates::Manifest manifest;
    if(!receive(&manifest,sizeof(manifest))||!Updates::valid(manifest)||manifest.release<=UpdateRelease)return;
    std::vector<unsigned char> image((size_t)manifest.bytes);
    if(!receive(image.data(),image.size())||!Updates::matches(manifest,image)||stop)return;
    unsigned char random[16];randombytes_buf(random,sizeof(random));char hex[33];sodium_bin2hex(hex,sizeof(hex),random,sizeof(random));
    const std::string suffix(hex);auto path=Updates::folder(Updates::executable())+L"\\.oi-update-"+std::wstring(suffix.begin(),suffix.end());
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)",SDDL_REVISION_1,&descriptor,nullptr))return;
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
    const bool created=CreateDirectoryW(path.c_str(),&attributes)!=FALSE;LocalFree(descriptor);
    if(!created)return;
    if(!Updates::writeNew(path+L"\\oi.exe",image.data(),(DWORD)image.size())||!Updates::writeNew(path+L"\\oi.update",&manifest,sizeof(manifest))){cleanup(path);return;}
    stage=path;
}
bool UpdateClient::prompt(HWND window) {
    if(prompted||!done.load()||stage.empty())return false;prompted=true;
    if(MessageBoxW(window,L"An oi update is ready. Restart now to install it?",L"Update available",MB_YESNO|MB_ICONINFORMATION)!=IDYES)return false;
    Updates::Manifest manifest;std::vector<unsigned char> image;
    const auto file=stage+L"\\oi.exe";
    if(!Updates::loadManifest(stage+L"\\oi.update",manifest)||manifest.release<=UpdateRelease||!Updates::readFile(file,image)||!Updates::matches(manifest,image))return false;
    installing=launch(file,L"--apply-update "+std::to_wstring(GetCurrentProcessId())+L" "+quote(Updates::executable()));
    if(!installing)MessageBoxW(window,L"Could not start the update. Your current version is unchanged.",L"Update failed",MB_OK|MB_ICONERROR);
    return installing;
}
int UpdateClient::startup() {
    if(sodium_init()<0)return 2;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return -1;
    struct Free {LPWSTR* p;~Free(){LocalFree(p);}} free{argv};
    const auto self=Updates::executable();const auto selfFolder=Updates::folder(self);
    if(argc==3 && wcscmp(argv[1],L"--cleanup-update")==0) {
        const std::wstring path=argv[2];
        if(stageName(path)&&Updates::folder(path)==selfFolder)for(int i=0;i<20;++i){cleanup(path);if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)break;Sleep(100);}
        return -1;
    }
    if(argc<2||wcscmp(argv[1],L"--apply-update")!=0)return -1;
    if(argc!=4||!stageName(selfFolder)||(GetFileAttributesW(selfFolder.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))return 2;
    const std::wstring target=argv[3];
    if(Updates::folder(target)!=Updates::folder(selfFolder)||!plainFile(target))return 2;
    Updates::Manifest manifest;std::vector<unsigned char> image;
    if(!Updates::loadManifest(selfFolder+L"\\oi.update",manifest)||manifest.release!=UpdateRelease||!Updates::readFile(self,image)||!Updates::matches(manifest,image))return 2;
    wchar_t* end=nullptr;unsigned long pid=wcstoul(argv[2],&end,10);if(!pid||*end)return 2;
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process){wchar_t path[32768];DWORD n=32768;bool same=QueryFullProcessImageNameW(process,0,path,&n)&&_wcsicmp(path,target.c_str())==0;
        if(!same){CloseHandle(process);return 2;}DWORD result=WaitForSingleObject(process,120000);CloseHandle(process);if(result!=WAIT_OBJECT_0)return 2;}
    else if(GetLastError()!=ERROR_INVALID_PARAMETER)return 2;
    const auto backup=target+L".previous";
    // Existing backup is retained rather than overwritten if it is not a regular file.
    DWORD attr=GetFileAttributesW(backup.c_str());
    if(attr!=INVALID_FILE_ATTRIBUTES && (!plainFile(backup)||!DeleteFileW(backup.c_str())))return 2;
    const auto ready=selfFolder+L"\\oi.ready";
    if(!CopyFileW(self.c_str(),ready.c_str(),TRUE))return 2;
    if(!ReplaceFileW(target.c_str(),ready.c_str(),backup.c_str(),0,nullptr,nullptr)){DeleteFileW(ready.c_str());return 2;}
    if(!launch(target,L"--cleanup-update "+quote(selfFolder))){
        if(!ReplaceFileW(target.c_str(),backup.c_str(),nullptr,0,nullptr,nullptr))
            MessageBoxW(nullptr,L"Could not restart or restore oi. Your previous executable is saved with the .previous suffix.",L"Update failed",MB_OK|MB_ICONERROR);
        else launch(target,L"");
        return 2;
    }
    return 0;
}
