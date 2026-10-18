#include "UpdateProtocol.h"
#include "UpdateClient.h"
#include "Network/AsyncResolver.h"
#include <shellapi.h>
#include <sddl.h>
#include <tlhelp32.h>

namespace {
std::wstring quote(const std::wstring& s){return L"\""+s+L"\"";}
HANDLE resumeMapping=nullptr;
HANDLE startupEvent=nullptr;
std::unique_ptr<SessionResume> startupResume;
std::wstring pendingCleanup;
bool recoveryLaunched=false;
uint64_t recoveredRelease=0;

bool launch(const std::wstring& exe,const std::wstring& args,HANDLE* child=nullptr,HANDLE retired=nullptr,HANDLE ready=nullptr) {
    std::wstring command=quote(exe)+L" "+args;
    HANDLE inherited[3];size_t count=0;
    if(retired){inherited[count++]=retired;command+=L" --retire-handle "+std::to_wstring(reinterpret_cast<uintptr_t>(retired));}
    if(resumeMapping)inherited[count++]=resumeMapping;
    if(ready){inherited[count++]=ready;command+=L" --ready-handle "+std::to_wstring(reinterpret_cast<uintptr_t>(ready));}
    STARTUPINFOEXW si={};si.StartupInfo.cb=count?sizeof(si):sizeof(STARTUPINFOW);PROCESS_INFORMATION pi={};
    SIZE_T bytes=0;std::vector<unsigned char> attributes;
    if(count){
        if(resumeMapping)command+=L" --resume-handle "+std::to_wstring(reinterpret_cast<uintptr_t>(resumeMapping));
        InitializeProcThreadAttributeList(nullptr,1,0,&bytes);attributes.resize(bytes);
        si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes))return false;
        if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,count*sizeof(HANDLE),nullptr,nullptr)){
            DeleteProcThreadAttributeList(si.lpAttributeList);return false;
        }
    }
    bool ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,count!=0,
        CREATE_NO_WINDOW|(count?EXTENDED_STARTUPINFO_PRESENT:0),nullptr,Updates::folder(exe).c_str(),&si.StartupInfo,&pi)!=FALSE;
    if(si.lpAttributeList)DeleteProcThreadAttributeList(si.lpAttributeList);
    if(!ok)return false;
    CloseHandle(pi.hThread);if(child)*child=pi.hProcess;else CloseHandle(pi.hProcess);return true;
}
bool launchConfirmed(const std::wstring& exe,const std::wstring& args,bool recovery=false) {
    Updates::File ready;
    if(!resumeMapping && !recovery){
        SECURITY_ATTRIBUTES attributes={sizeof(attributes),nullptr,TRUE};
        ready.handle=CreateEventW(&attributes,TRUE,FALSE,nullptr);
        if(!ready.handle){ready.handle=INVALID_HANDLE_VALUE;return false;}
    }
    HANDLE child=nullptr;
    bool resumed=launch(exe,args,&child,nullptr,ready.handle==INVALID_HANDLE_VALUE?nullptr:ready.handle);
    if(resumed && ready.handle!=INVALID_HANDLE_VALUE){
        HANDLE waits[]={ready.handle,child};
        resumed=WaitForMultipleObjects(2,waits,FALSE,30000)==WAIT_OBJECT_0;
    }
    if(resumed && resumeMapping){
        auto view=static_cast<SessionResume*>(MapViewOfFile(resumeMapping,FILE_MAP_WRITE,0,0,sizeof(SessionResume)));
        resumed=false;
        if(view){
            const auto deadline=GetTickCount64()+30000;
            while(GetTickCount64()<deadline){
                const LONG status=InterlockedCompareExchange(&view->started,0,0);
                if(status){resumed=status==1;break;}
                if(WaitForSingleObject(child,50)==WAIT_OBJECT_0)break;
            }
            InterlockedExchange(&view->started,0);UnmapViewOfFile(view);
        }
    }
    if(!resumed && child && WaitForSingleObject(child,5000)==WAIT_TIMEOUT){TerminateProcess(child,2);WaitForSingleObject(child,5000);}
    if(child)CloseHandle(child);
    return resumed;
}
bool restartPrevious(const std::wstring& target,const std::wstring& stage) {
    if(resumeMapping){
        auto view=static_cast<SessionResume*>(MapViewOfFile(resumeMapping,FILE_MAP_WRITE,0,0,sizeof(SessionResume)));
        if(view){view->updateFailed=1;InterlockedExchange(&view->started,0);UnmapViewOfFile(view);}
    }
    // Older disconnected clients have no startup acknowledgement protocol.
    recoveryLaunched=launchConfirmed(target,L"--cleanup-update "+quote(stage),true);
    if(recoveryLaunched){
        DeleteFileW((stage+L"\\oi.previous").c_str());
        DeleteFileW((stage+L"\\oi.rollback").c_str());
    }
    return recoveryLaunched;
}
void retainCleanupFile(Updates::File& file,const std::wstring& path) {
    file.handle=CreateFileW(path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
}
bool readRetiredFile(Updates::File& file,std::vector<unsigned char>& bytes) {
    BY_HANDLE_FILE_INFORMATION info={};LARGE_INTEGER size={},start={};
    if(!GetFileInformationByHandle(file.handle,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file.handle,&size) || size.QuadPart<=0 || uint64_t(size.QuadPart)>Updates::MaxBytes ||
        !SetFilePointerEx(file.handle,start,nullptr,FILE_BEGIN))return false;
    bytes.resize(size_t(size.QuadPart));DWORD read=0;
    return ReadFile(file.handle,bytes.data(),DWORD(bytes.size()),&read,nullptr) && read==bytes.size();
}
void deleteRetiredFile(Updates::File& file,const std::wstring& installed) {
    if(file.handle==INVALID_HANDLE_VALUE)return;
    Updates::File current;current.handle=CreateFileW(installed.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION oldInfo={},newInfo={};
    if(!GetFileInformationByHandle(file.handle,&oldInfo) || !GetFileInformationByHandle(current.handle,&newInfo) ||
        (oldInfo.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return;
    if(oldInfo.dwVolumeSerialNumber==newInfo.dwVolumeSerialNumber &&
        oldInfo.nFileIndexHigh==newInfo.nFileIndexHigh && oldInfo.nFileIndexLow==newInfo.nFileIndexLow)return;
    FILE_DISPOSITION_INFO disposition={TRUE};SetFileInformationByHandle(file.handle,FileDispositionInfo,&disposition,sizeof(disposition));
}
bool installerParent(DWORD pid) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W entry={};entry.dwSize=sizeof(entry);bool matches=false;
    if(Process32FirstW(snapshot,&entry))do {
        if(entry.th32ProcessID==GetCurrentProcessId()){
            matches=entry.th32ParentProcessID==pid;break;
        }
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return matches;
}
bool stageName(const std::wstring& path) {
    // Retain the staging format so older clients can install renamed releases.
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
UpdateClient::UpdateClient(const std::string& server):address(server),skippedRelease(recoveredRelease){poll();}
void UpdateClient::poll() {
    static ULONGLONG nextCleanup=0;
    if(!pendingCleanup.empty() && GetTickCount64()>=nextCleanup){
        cleanup(pendingCleanup);
        if(GetFileAttributesW(pendingCleanup.c_str())==INVALID_FILE_ATTRIBUTES)pendingCleanup.clear();
        nextCleanup=GetTickCount64()+1000;
    }
    if(worker.joinable()){
        if(!done.load())return;
        worker.join();
        if(checkSucceeded){retrySeconds=5;nextCheck=GetTickCount64()+30*60*1000+randombytes_uniform(60001);}
        else {nextCheck=GetTickCount64()+retrySeconds*1000+randombytes_uniform(retrySeconds*1000+1);retrySeconds=(std::min)(retrySeconds*2,150u);}
    }
    if(installing || GetTickCount64()<nextCheck)return;
    // Retain the verified image while the room drains and clients reconnect.
    if(!stage.empty() && release!=skippedRelease)return;
    if(!stage.empty()){cleanup(stage);stage.clear();}
    release=0;protocol=0;claimed=false;done=false;
    checkSucceeded=false;
    worker=std::thread([this]{try {checkSucceeded=download(address);} catch(const std::bad_alloc&) {} done.store(true);});
}
bool UpdateClient::requestRelease(uint64_t minimum) {
    if(installing || readyRelease()>=minimum)return true;
    if(!done.load())return false;
    const auto now=GetTickCount64();if(now<nextForcedCheck)return true;
    nextForcedCheck=now+30000;
    if(worker.joinable())worker.join();
    if(!stage.empty()){cleanup(stage);stage.clear();}
    nextCheck=0;poll();return true;
}
std::unique_ptr<SessionResume> UpdateClient::takeResume() {
    return std::move(startupResume);
}
UpdateClient::~UpdateClient(){
    stop=true;
    if(worker.joinable())worker.join();if(!installing&&!stage.empty())cleanup(stage);
}
bool UpdateClient::recoveredStartup(){return recoveryLaunched;}
void UpdateClient::completeResume(bool success) {
    if(resumeMapping){
        auto view=static_cast<SessionResume*>(MapViewOfFile(resumeMapping,FILE_MAP_WRITE,0,0,sizeof(SessionResume)));
        if(view){InterlockedExchange(&view->started,success?1:-1);UnmapViewOfFile(view);}
        CloseHandle(resumeMapping);resumeMapping=nullptr;
    }
    if(startupEvent){if(success)SetEvent(startupEvent);CloseHandle(startupEvent);startupEvent=nullptr;}
    if(!success)pendingCleanup.clear();
}
bool UpdateClient::download(std::string address,bool queued) {
    const std::string originalAddress=address;
    sockaddr_in remote={};remote.sin_family=AF_INET;remote.sin_port=htons(778);
    const auto colon=address.find(':');if(colon!=std::string::npos){
        const auto port=address.substr(colon+1);char* end=nullptr;unsigned long n=strtoul(port.c_str(),&end,10);
        if(port.empty()||*end||n<1||n>65535)return false;remote.sin_port=htons((u_short)n);address.resize(colon);
    }
    AsyncResolver resolver;resolver.start(address);int result=0;
    while(!stop && !(result=resolver.poll(remote)))Sleep(20);
    if(stop||result<0)return false;
    SOCKET socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(socket==INVALID_SOCKET)return false;
    struct Close {SOCKET s;~Close(){closesocket(s);}} close{socket};
    u_long mode=1;if(ioctlsocket(socket,FIONBIO,&mode))return false;
    ULONGLONG started=GetTickCount64();
    auto wait=[&](bool writing,ULONGLONG deadline){
        while(!stop && GetTickCount64()<deadline){fd_set set;FD_ZERO(&set);FD_SET(socket,&set);timeval timeout={0,100000};
            int n=select(0,writing?nullptr:&set,writing?&set:nullptr,nullptr,&timeout);if(n>0)return true;if(n<0)return false;}
        return false;
    };
    if(connect(socket,(sockaddr*)&remote,sizeof(remote))==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)return false;
    if(!wait(true,started+10000))return false;
    int error=0,size=sizeof(error);if(getsockopt(socket,SOL_SOCKET,SO_ERROR,(char*)&error,&size)||error)return false;
    if(send(socket,queued?"Q":"U",1,0)!=1)return false;
    if(queued){
        const auto queueDeadline=GetTickCount64()+30*60*1000;
        unsigned notices=0;
        for(;;){
            if(!wait(false,(std::min)(queueDeadline,GetTickCount64()+15000)))return false;
            char state=0;const int n=recv(socket,&state,1,0);
            if(n<0 && WSAGetLastError()==WSAEWOULDBLOCK)continue;
            // Older directories understand only U; retry that protocol once.
            if(n<=0 && notices==0 && !stop){closesocket(socket);close.s=INVALID_SOCKET;return download(originalAddress,false);}
            if(n!=1)return false;
            if(state=='U')break;
            if(state!='W' || ++notices>1024)return false;
        }
        started=GetTickCount64();
    }
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
    if(!receive(&manifest,sizeof(manifest))||!Updates::valid(manifest))return false;
    if(manifest.release<=UpdateRelease||manifest.release==skippedRelease)return true;
    std::vector<unsigned char> image((size_t)manifest.bytes);
    if(!receive(image.data(),image.size())||!Updates::matches(manifest,image)||stop)return false;
    unsigned char random[16];randombytes_buf(random,sizeof(random));char hex[33];sodium_bin2hex(hex,sizeof(hex),random,sizeof(random));
    const std::string suffix(hex);auto path=Updates::folder(Updates::executable())+L"\\.oi-update-"+std::wstring(suffix.begin(),suffix.end());
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)",SDDL_REVISION_1,&descriptor,nullptr))return false;
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
    const bool created=CreateDirectoryW(path.c_str(),&attributes)!=FALSE;LocalFree(descriptor);
    if(!created)return false;
    if(!Updates::writeNew(path+L"\\oi.exe",image.data(),(DWORD)image.size())||!Updates::writeNew(path+L"\\oi.update",&manifest,sizeof(manifest))){cleanup(path);return false;}
    stage=path;
    HMODULE module=LoadLibraryExW((path+L"\\oi.exe").c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(module){
        HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(203),MAKEINTRESOURCEW(10));
        if(resource && SizeofResource(module,resource)==sizeof(uint32_t))
            protocol=*static_cast<const uint32_t*>(LockResource(LoadResource(module,resource)));
        FreeLibrary(module);
    }
    release=manifest.release;return true;
}
bool UpdateClient::claimUpdate() {
    if(claimed || !readyRelease() || stage.empty() || release==skippedRelease)return false;
    claimed=true;return true;
}
void UpdateClient::skipRelease() {
    if(!done.load() || installing)return;
    if(worker.joinable())worker.join();
    skippedRelease=release;claimed=true;
    if(!stage.empty()){cleanup(stage);stage.clear();}
    release=0;protocol=0;
}
const char* UpdateClient::installBlockReason() const {
    auto target=Updates::executable();
    if(!plainFile(target))return "Update postponed: the executable is missing or is not a regular file.";
    if(!stageName(stage) || Updates::folder(stage)!=Updates::folder(target) ||
        (GetFileAttributesW(stage.c_str()) & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))!=FILE_ATTRIBUTE_DIRECTORY)
        return "Update postponed: the update folder is invalid.";
    const auto installed=Updates::folder(target)+L"\\subnet.exe";
    if(_wcsicmp(installed.c_str(),target.c_str())!=0 && GetFileAttributesW(installed.c_str())!=INVALID_FILE_ATTRIBUTES){
        Updates::Manifest manifest;std::vector<unsigned char> image;
        if(!Updates::loadManifest(stage+L"\\oi.update",manifest) ||
            !Updates::readFile(installed,image) || !Updates::matches(manifest,image))
            return "Update postponed: a different subnet.exe already exists in this folder.";
        target=installed;
    }
    const auto backup=target+L".previous";
    if(GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES && !plainFile(backup))
        return "Update postponed: the backup path is not a regular file.";
    Updates::File targetFile;retainCleanupFile(targetFile,target);
    if(targetFile.handle==INVALID_HANDLE_VALUE)return "Update postponed: the executable is locked or cannot be replaced.";
    if(GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES){
        Updates::File backupFile;retainCleanupFile(backupFile,backup);
        if(backupFile.handle==INVALID_HANDLE_VALUE)return "Update postponed: the previous backup is locked.";
    }
    return nullptr;
}
bool UpdateClient::install(const SessionResume* resume) {
    if(installing||!readyRelease()||stage.empty()||installBlockReason())return false;
    Updates::Manifest manifest;std::vector<unsigned char> image;
    const auto file=stage+L"\\oi.exe";
    Updates::File verified;
    if(!Updates::loadManifest(stage+L"\\oi.update",manifest)||manifest.release<=UpdateRelease||!Updates::readFile(file,image,Updates::MaxBytes,&verified)||!Updates::matches(manifest,image))return false;
    wchar_t running[32768];DWORD length=_countof(running);Updates::File retired;
    if(!QueryFullProcessImageNameW(GetCurrentProcess(),0,running,&length))return false;
    retainCleanupFile(retired,running);
    if(retired.handle==INVALID_HANDLE_VALUE || !SetHandleInformation(retired.handle,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT))return false;
    if(resume){
        if(!resume->valid())return false;
        SECURITY_ATTRIBUTES attributes={sizeof(attributes),nullptr,TRUE};
        resumeMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE,0,sizeof(SessionResume),nullptr);
        if(!resumeMapping)return false;
        void* view=MapViewOfFile(resumeMapping,FILE_MAP_WRITE,0,0,sizeof(SessionResume));
        if(!view){CloseHandle(resumeMapping);resumeMapping=nullptr;return false;}
        memcpy(view,resume,sizeof(SessionResume));UnmapViewOfFile(view);
    }
    installing=launch(file,L"--apply-update "+std::to_wstring(GetCurrentProcessId())+L" "+quote(Updates::executable()),nullptr,retired.handle);
    if(resumeMapping){CloseHandle(resumeMapping);resumeMapping=nullptr;}

    return installing;
}
int UpdateClient::startup() {
    if(sodium_init()<0)return 2;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return -1;
    struct Free {LPWSTR* p;~Free(){LocalFree(p);}} free{argv};
    if(argc>=3 && wcscmp(argv[argc-2],L"--resume-handle")==0){
        wchar_t* end=nullptr;auto value=_wcstoui64(argv[argc-1],&end,10);
        if(!value || !end || *end)return 2;
        resumeMapping=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
        const void* view=MapViewOfFile(resumeMapping,FILE_MAP_READ,0,0,sizeof(SessionResume));
        if(!view)return 2;
        startupResume=std::make_unique<SessionResume>();memcpy(startupResume.get(),view,sizeof(SessionResume));UnmapViewOfFile(view);
        if(!startupResume->valid())return 2;
        if(!SetHandleInformation(resumeMapping,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT))return 2;
        argc-=2;
    }
    Updates::File oldExecutable;
    if(argc>=3 && wcscmp(argv[argc-2],L"--ready-handle")==0){
        wchar_t* end=nullptr;auto value=_wcstoui64(argv[argc-1],&end,10);
        if(!value || !end || *end)return 2;
        startupEvent=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
        if(!SetHandleInformation(startupEvent,HANDLE_FLAG_INHERIT,0))return 2;
        argc-=2;
    }
    if(argc>=3 && wcscmp(argv[argc-2],L"--retire-handle")==0){
        wchar_t* end=nullptr;auto value=_wcstoui64(argv[argc-1],&end,10);
        if(!value || !end || *end)return 2;
        oldExecutable.handle=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
        if(!SetHandleInformation(oldExecutable.handle,HANDLE_FLAG_INHERIT,0))return 2;
        argc-=2;
    }
    const auto self=Updates::executable();const auto selfFolder=Updates::folder(self);
    if(argc==3 && wcscmp(argv[1],L"--cleanup-update")==0) {
        const std::wstring path=argv[2];
        if(stageName(path)&&Updates::folder(path)==selfFolder){
            if(!resumeMapping && !startupEvent){
                Updates::Manifest failed;
                if(Updates::loadManifest(path+L"\\oi.update",failed))recoveredRelease=failed.release;
            }
            pendingCleanup=path;
        }
        return -1;
    }
    if(argc<2||wcscmp(argv[1],L"--apply-update")!=0)return -1;
    if(argc!=4||!stageName(selfFolder)||(GetFileAttributesW(selfFolder.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))return 2;
    std::wstring target=argv[3];
    if(Updates::folder(target)!=Updates::folder(selfFolder)||!plainFile(target))return 2;
    Updates::Manifest manifest;std::vector<unsigned char> image;
    if(!Updates::loadManifest(selfFolder+L"\\oi.update",manifest)||manifest.release!=UpdateRelease||!Updates::readFile(self,image)||!Updates::matches(manifest,image))return 2;
    wchar_t* end=nullptr;unsigned long pid=wcstoul(argv[2],&end,10);if(!pid||*end)return 2;
    // Bind the request to the process that launched this installer. Its image
    // may have been renamed while running to make room for a newer build.
    if(!installerParent(pid))return 2;
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process){
        FILETIME parentCreated,parentExit,parentKernel,parentUser,selfCreated,selfExit,selfKernel,selfUser;
        if(!GetProcessTimes(process,&parentCreated,&parentExit,&parentKernel,&parentUser) ||
            !GetProcessTimes(GetCurrentProcess(),&selfCreated,&selfExit,&selfKernel,&selfUser) ||
            CompareFileTime(&parentCreated,&selfCreated)>0){CloseHandle(process);return 2;}
        wchar_t running[32768];DWORD length=_countof(running);
        if(oldExecutable.handle==INVALID_HANDLE_VALUE && QueryFullProcessImageNameW(process,0,running,&length) &&
            _wcsicmp(Updates::folder(running).c_str(),Updates::folder(target).c_str())==0)
            retainCleanupFile(oldExecutable,running);
        const DWORD result=WaitForSingleObject(process,120000);
        CloseHandle(process);if(result!=WAIT_OBJECT_0)return 2;
    }
    else if(GetLastError()!=ERROR_INVALID_PARAMETER)return 2;
    auto restartRunning=[&]{
        wchar_t path[32768];
        const DWORD length=GetFinalPathNameByHandleW(oldExecutable.handle,path,_countof(path),FILE_NAME_NORMALIZED);
        std::wstring running=length && length<_countof(path)?std::wstring(path,length):target;
        if(running.compare(0,8,L"\\\\?\\UNC\\")==0)running=L"\\\\"+running.substr(8);
        else if(running.compare(0,4,L"\\\\?\\")==0)running.erase(0,4);
        if(oldExecutable.handle!=INVALID_HANDLE_VALUE){CloseHandle(oldExecutable.handle);oldExecutable.handle=INVALID_HANDLE_VALUE;}
        return restartPrevious(running,selfFolder);
    };
    const auto installed=Updates::folder(target)+L"\\subnet.exe";
    if(_wcsicmp(installed.c_str(),target.c_str())==0)target=installed;
    else if(GetFileAttributesW(installed.c_str())!=INVALID_FILE_ATTRIBUTES){
        std::vector<unsigned char> existing;
        if(!Updates::readFile(installed,existing) || !Updates::matches(manifest,existing)){
            restartRunning();
            return 2;
        }
        target=installed;
    }
    const auto backup=target+L".previous";
    // Do not remove the existing backup until replacement succeeds.
    DWORD attr=GetFileAttributesW(backup.c_str());
    if(attr!=INVALID_FILE_ATTRIBUTES && !plainFile(backup)){restartRunning();return 2;}
    std::vector<unsigned char> previous;
    if(!(oldExecutable.handle!=INVALID_HANDLE_VALUE?readRetiredFile(oldExecutable,previous):Updates::readFile(target,previous))){
        restartRunning();return 2;
    }
    const auto rollback=selfFolder+L"\\oi.rollback";
    if(!Updates::writeNew(rollback,previous.data(),DWORD(previous.size()))){restartRunning();return 2;}
    auto restore=[&](const std::wstring& saved,const std::wstring& destination){
        if(!ReplaceFileW(destination.c_str(),saved.c_str(),nullptr,0,nullptr,nullptr)){
            if(GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES ||
                !MoveFileExW(saved.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))return restartRunning();
        }
        if(!restartPrevious(destination,selfFolder))return restartRunning();
        deleteRetiredFile(oldExecutable,destination);
        return true;
    };
    const auto ready=selfFolder+L"\\oi.ready";
    const auto temporaryBackup=selfFolder+L"\\oi.previous";
    if(!Updates::writeNew(ready,image.data(),(DWORD)image.size())){restore(rollback,target);return 2;}
    if(!ReplaceFileW(target.c_str(),ready.c_str(),temporaryBackup.c_str(),0,nullptr,nullptr)){
        DeleteFileW(ready.c_str());
        restore(rollback,target);return 2;
    }
    if(!MoveFileExW(rollback.c_str(),backup.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        restore(rollback,target);
        return 2;
    }
    if(installed!=target && !MoveFileExW(target.c_str(),installed.c_str(),MOVEFILE_WRITE_THROUGH)){
        restore(backup,target);
        return 2;
    }
    Updates::File backupFile;retainCleanupFile(backupFile,backup);
    const bool resumed=launchConfirmed(installed,L"--cleanup-update "+quote(selfFolder));
    if(!resumed){
        if(backupFile.handle!=INVALID_HANDLE_VALUE){CloseHandle(backupFile.handle);backupFile.handle=INVALID_HANDLE_VALUE;}
        restore(backup,installed);
        return 2;
    }
    // Only a successful startup acknowledgement authorizes cleanup.
    // Handles identify these exact files even if their paths are reused.
    deleteRetiredFile(backupFile,installed);
    deleteRetiredFile(oldExecutable,installed);
    DeleteFileW(temporaryBackup.c_str());DeleteFileW(rollback.c_str());
    return 0;
}
