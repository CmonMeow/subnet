#pragma once
#include "NetworkProtocol.h"
#include "FileIoQueue.h"
#include "TransferProgress.h"
#include "Screenshot.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>

// Protocol state stays on the application thread; disk work uses one bounded worker.
// The relay never parses this plaintext protocol or opens a file.
class FileTransfers
{
public:
    enum : unsigned char { Offer = 1, Accept, Chunk, Ack, Finish, Done, Cancel, ImageOffer, CompressedImageOffer };
    static constexpr unsigned ChunkBytes = 8192;
    // Shared by all uploads, so four recipients cannot multiply the backlog.
    static constexpr unsigned FlightBytes = 1024 * 1024;
    static constexpr unsigned __int64 MaxFileBytes = 4ULL * 1024 * 1024 * 1024;
    static constexpr unsigned MaxPacketBytes = ChunkBytes + 128;
    static constexpr size_t MaxOffers = 1024, MaxOffersPerPeer = 128;
    using Key = std::pair<int, string>;
    using Send = std::function<bool(int, const vector<unsigned char>&)>;
    using Notice = std::function<void(const string&, bool)>;
    using Announce = std::function<void(int, const string&)>;
private:
    struct File {
        std::shared_ptr<Screenshot> image;
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::wstring temporary;
        std::atomic<int> phase{0}; // 0 active, 1 committing a save, 2 cancelled
        ~File() { close(); if (!temporary.empty()) DeleteFileW(temporary.c_str()); }
        void close() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
    };
    struct Source {
        std::shared_ptr<File> file;
        string name;
        unsigned __int64 size = 0;
        ULONGLONG expires = 0;
        std::set<int> peers;
    };
    struct Download {
        string name, token, state = "save";
        unsigned __int64 size = 0, offset = 0, queuedOffset = 0;
        bool finishing = false, image = false, compressed = false;
        std::shared_ptr<Screenshot> readyImage;
        ULONGLONG touched = 0;
        std::shared_ptr<File> file;
        std::wstring destination;
        crypto_generichash_state hash;
        TransferProgress progress;
    };
    struct Upload {
        std::shared_ptr<Source> source;
        string token;
        unsigned __int64 offset = 0, acknowledged = 0;
        bool finishing = false, reading = false;
        vector<unsigned char> buffer;
        size_t cursor = 0;
        ULONGLONG touched = 0;
        crypto_generichash_state hash;
        TransferProgress progress;
    };
    FileIoQueue io; // Outlives maps and their deferred handle cleanup.
    std::map<string, std::shared_ptr<Source>> sources;
    std::map<Key, Download> downloads;
    std::map<Key, Upload> uploads;
    struct PendingControl { unsigned char type; unsigned __int64 offset; ULONGLONG expires; };
    std::map<std::pair<Key,string>,PendingControl> pendingControls;
    std::pair<Key,string> lastControl;
    Key lastServed;
    Send send;
    Notice notice;
    Announce announce;

    static string randomId() {
        unsigned char value[16]; randombytes_buf(value, sizeof(value));
        char hex[33]; sodium_bin2hex(hex, sizeof(hex), value, sizeof(value)); return hex;
    }
    static bool validId(const string& id) {
        if (id.size() != 32) return false;
        for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    static NetPacket header(unsigned char type, const string& id, const string& token = "") {
        NetPacket raw; raw.putUInt8(type); raw.putString(id, 32); raw.putString(token, 32); return raw;
    }
    bool transmit(int peer, const NetPacket& raw) {
        return send(peer, vector<unsigned char>(raw.data(), raw.data() + raw.size()));
    }
    void discardControls(const Key& key) {
        for(auto it=pendingControls.begin();it!=pendingControls.end();)
            if(it->first.first==key)it=pendingControls.erase(it);else ++it;
    }
    void control(unsigned char type, const Key& key, const string& token, unsigned __int64 offset = 0) {
        // ACKs are cumulative; terminal controls supersede any earlier state.
        const auto pendingKey=std::make_pair(key,token);
        if(type==Cancel&&token.empty())discardControls(key);
        auto previous=pendingControls.find(pendingKey);
        if(previous!=pendingControls.end() && (previous->second.type==Cancel||previous->second.type==Done) && type==Ack)return;
        auto raw=header(type,key.second,token);if(type==Ack)raw.putUInt64(offset);
        if(transmit(key.first,raw)){pendingControls.erase(pendingKey);return;}
        // Unsolicited invalid accepts can generate cancels: do not let them fill memory.
        if(type==Cancel&&previous==pendingControls.end()&&pendingControls.size()>=1024)return;
        pendingControls[pendingKey]={type,offset,GetTickCount64()+30000};
    }
    void retryControls(ULONGLONG now) {
        const size_t work=(std::min)(pendingControls.size(),size_t(128));
        for(size_t i=0;i<work&&!pendingControls.empty();++i){
            auto it=pendingControls.upper_bound(lastControl);if(it==pendingControls.end())it=pendingControls.begin();
            lastControl=it->first;
            const auto& key=it->first.first;const auto& token=it->first.second;const auto& pending=it->second;
            if(now>=pending.expires){pendingControls.erase(it);continue;}
            auto raw=header(pending.type,key.second,token);if(pending.type==Ack)raw.putUInt64(pending.offset);
            if(transmit(key.first,raw))pendingControls.erase(it);
        }
    }

    std::shared_ptr<File> newFile() {
        return std::shared_ptr<File>(new File,[this](File* file){
            io.submit([file]() -> FileIoQueue::Completion { delete file; return {}; },true);
        });
    }
    bool releaseDownload(Download& d) {
        if (d.file) {
            int active = 0;
            if (!d.file->phase.compare_exchange_strong(active,2) && active == 1) return false;
            d.file.reset();
        }
        return true;
    }
    void failDownload(const Key& key, Download& d, const string& reason, bool tellPeer = true) {
        // A save already committing cannot be cancelled or reported as failed.
        if (!releaseDownload(d)) return;
        if (tellPeer && !d.token.empty()) control(Cancel, key, d.token);
        d.state = reason; d.touched = GetTickCount64();
        notice(d.name + ": " + reason, true);
    }
    static bool validName(const string& name) {
        if (name.empty() || name.size() > 200 || name.back() == '.' || name.back() == ' ') return false;
        for (unsigned char c : name) if (c < 32 || c == 127 || strchr("\\/:*?\"<>|", c)) return false;
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), (int)name.size(), nullptr, 0) > 0;
    }
public:
    std::function<void(std::shared_ptr<const Screenshot>)> viewImage;
    FileTransfers(Send s, Notice n, Announce a) : send(std::move(s)), notice(std::move(n)), announce(std::move(a)) {}
    ~FileTransfers() { clear(); }
    static bool localPath(const std::wstring& path) {
        return path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && path[2] == L'\\' && path.find(L':', 2) == std::wstring::npos &&
            GetDriveTypeW(path.substr(0,3).c_str()) != DRIVE_REMOTE;
    }
    static string utf8(const std::wstring& value) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
        string result(n, '\0'); if (n) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), &result[0], n, nullptr, nullptr); return result;
    }
    static std::wstring wide(const string& value) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
        std::wstring result(n, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), &result[0], n); return result;
    }
    static string displayName(const string& name) {
        string result; for (unsigned char c : name) result += c >= 32 && c < 127 ? (char)c : '?'; return result;
    }
    bool offer(const std::wstring& path, const vector<int>& peers) {
        if (peers.empty()) { notice("No recipients are ready for file transfers.", true); return false; }
        if (!localPath(path)) { notice("Path is not a local file.", true); return false; }
        auto source = std::make_shared<Source>(); source->file = newFile();
        source->name = utf8(path.substr(path.find_last_of(L"\\/") + 1));
        if (!validName(source->name)) { notice("Unsupported filename (maximum 200 UTF-8 bytes).", true); return false; }
        source->peers.insert(peers.begin(),peers.end());
        source->expires = GetTickCount64() + 600000;
        const string id = randomId(); auto file = source->file;
        if (!io.submit([this,file,path,id]() -> FileIoQueue::Completion {
            // Deny concurrent writers/deletion throughout all recipients' transfers.
            file->handle = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION info = {}; LARGE_INTEGER size = {};
            bool ok = file->handle != INVALID_HANDLE_VALUE && GetFileType(file->handle) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(file->handle,&info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) &&
                GetFileSizeEx(file->handle,&size) && size.QuadPart >= 0 && size.QuadPart <= MaxFileBytes;
            return [this,id,ok,size] {
                auto it = sources.find(id); if (it == sources.end()) return;
                auto source = it->second;
                if (!ok) { sources.erase(it); notice("Cannot offer file: inaccessible, in use, or exceeds 4,294,967,296 bytes.",true); return; }
                source->size = size.QuadPart; source->expires = GetTickCount64()+600000;
                auto raw = header(Offer,id); raw.putUInt64(source->size); raw.putString(source->name,200);
                for (auto peer=source->peers.begin();peer!=source->peers.end();)
                    if (!transmit(*peer,raw)) peer=source->peers.erase(peer); else ++peer;
                if (source->peers.empty()) { sources.erase(it); notice("File recipients are unavailable.",true); return; }
                announce(-1,id);
            };
        })) { notice("File I/O queue is busy.",true); return false; }
        sources[id] = source;
        return true;
    }
    bool offerImage(std::shared_ptr<Screenshot> image, const vector<int>& peers) {
        if (!image || !image->valid()) return false;
        if (peers.empty()) { notice("No recipients are ready for images.",true); return false; }
        size_t images=0;for(const auto& entry:sources)if(entry.second->file->image)++images;
        if(images>=16){notice("Cancel an image offer before pasting another.",true);return false;}
        auto source=std::make_shared<Source>();source->file=newFile();source->file->image=image;
        source->name="image";source->expires=GetTickCount64()+600000;source->peers.insert(peers.begin(),peers.end());
        const string id=randomId();sources[id]=source;
        if(!io.submit([this,id,image=std::move(image)]() mutable -> FileIoQueue::Completion {
            auto packed=Screenshot::compress(std::move(image));
            return [this,id,packed=std::move(packed)] {
                auto it=sources.find(id);if(it==sources.end())return;
                if(!packed){sources.erase(it);notice("Could not prepare image.",true);return;}
                auto& source=*it->second;source.file->image=packed;source.size=packed->bytes.size();
                source.expires=GetTickCount64()+600000;
                auto raw=header(packed->compressed()?CompressedImageOffer:ImageOffer,id);
                raw.putUInt64(source.size);raw.putString(source.name,200);
                for(auto peer=source.peers.begin();peer!=source.peers.end();)
                    if(!transmit(*peer,raw))peer=source.peers.erase(peer);else ++peer;
                if(source.peers.empty()){sources.erase(it);notice("Image recipients are unavailable.",true);return;}
                announce(-1,id);
            };
        })) { sources.erase(id);notice("File I/O queue is busy.",true);return false; }
        return true;
    }
    bool isImage(const Key& key) const {
        if(key.first==-1){auto it=sources.find(key.second);return it!=sources.end() && !!it->second->file->image;}
        auto it=downloads.find(key);return it!=downloads.end() && it->second.image;
    }
    bool view(const Key& key) {
        auto it=downloads.find(key);if(it==downloads.end() || !it->second.image)return false;
        auto& d=it->second;
        if(d.readyImage){if(viewImage)viewImage(d.readyImage);return true;}
        if(d.file || d.state!="save")return false;
        size_t active=0;for(const auto& entry:downloads)if(entry.second.file)++active;
        if(active>=4){notice("At most four downloads can run.",true);return false;}
        auto image=Screenshot::allocate((size_t)d.size);
        if(!image) {
            // Reclaim cached images, but keep any image currently in the viewer.
            for(auto& entry:downloads)if(entry.second.readyImage && entry.second.readyImage.use_count()==1) {
                entry.second.readyImage.reset();entry.second.state="expired";
            }
            image=Screenshot::allocate((size_t)d.size);
        }
        if(!image){notice("Screenshot memory is full; close images or cancel offers.",true);return false;}
        d.file=newFile();d.file->image=std::move(image);d.token=randomId();d.offset=d.queuedOffset=0;
        d.finishing=false;d.touched=GetTickCount64();crypto_generichash_init(&d.hash,nullptr,0,32);
        control(Accept,key,d.token);return true;
    }
    std::wstring suggestedName(const Key& key) const {
        auto it = downloads.find(key); if (it == downloads.end() || it->second.file || it->second.state != "save") return L"";
        const auto name = wide(it->second.name);
        auto stem = name.substr(0,name.find(L'.'));
        while (!stem.empty() && stem.back()==L' ') stem.pop_back();
        const bool device = !_wcsicmp(stem.c_str(),L"CON") || !_wcsicmp(stem.c_str(),L"PRN") ||
            !_wcsicmp(stem.c_str(),L"AUX") || !_wcsicmp(stem.c_str(),L"NUL") ||
            (stem.size()==4 && (!_wcsnicmp(stem.c_str(),L"COM",3) || !_wcsnicmp(stem.c_str(),L"LPT",3)) &&
                ((stem[3]>=L'1' && stem[3]<=L'9') || stem[3]==0xb9 || stem[3]==0xb2 || stem[3]==0xb3));
        return device ? L"download-"+name : name;
    }
    string label(const Key& key) const {
        string name, state; unsigned __int64 bytes;
        if (key.first == -1) {
            auto it = sources.find(key.second); if (it == sources.end()) return "File offer withdrawn or expired";
            name = it->second->name; bytes = it->second->size; state = "offered/cancel";
            bool sending=false,unknown=false;long long remaining=0;
            for(const auto& upload:uploads)if(upload.first.second==key.second){sending=true;auto eta=upload.second.progress.remaining(bytes,upload.second.acknowledged,GetTickCount64());if(eta<0)unknown=true;else remaining=(std::max)(remaining,eta);}
            if(sending)state=TransferProgress::eta(unknown?-1:remaining)+" cancel";
        } else {
            auto it = downloads.find(key); if (it == downloads.end()) return "File offer expired";
            const auto& d = it->second; name = d.name; bytes = d.size;
            state = d.file ? std::to_string(d.size ? (unsigned long long)d.offset * 100 / d.size : 0) + "% "+(d.finishing?string(d.image?"validating":"saving"):TransferProgress::eta(d.progress.remaining(d.size,d.offset,GetTickCount64())))+" cancel" : d.state;
        }
        name = displayName(name); if (name.size() > 25) name = name.substr(0,12) + "..." + name.substr(name.size()-10);
        char size[32];
        // Windows labels binary file-size units KB/MB/GB and truncates the displayed fraction.
        if (bytes >= 1073741824ULL) snprintf(size,sizeof(size),"%.2f GB",(bytes*100/1073741824ULL)/100.0);
        else if (bytes >= 1048576) snprintf(size,sizeof(size),"%.2f MB",(bytes*100/1048576)/100.0);
        else if (bytes >= 1024) snprintf(size,sizeof(size),"%.2f KB",(bytes*100/1024)/100.0);
        else snprintf(size,sizeof(size),"%llu B",bytes);
        if (isImage(key)) return "[image] (" + string(size) + ") [" + (state=="save" || state=="view" ? "view" : state) + "]";
        return name + " (" + size + ") [" + state + "]";
    }
    void withdraw(const string& id) {
        auto source = sources.find(id); if (source == sources.end()) return;
        for (int peer : source->second->peers) control(Cancel,{peer,id},"");
        for (auto it=uploads.begin(); it!=uploads.end();) if (it->first.second==id) it=uploads.erase(it); else ++it;
        sources.erase(source);
    }
    bool active(const Key& key) const { auto it = downloads.find(key); return it != downloads.end() && !!it->second.file; }
    void cancel(const Key& key) { auto it = downloads.find(key); if (it != downloads.end() && it->second.file) failDownload(key, it->second, "cancelled"); }
    bool accept(const Key& key, const std::wstring& destination) {
        auto it = downloads.find(key);
        if (it == downloads.end() || it->second.file || it->second.image || it->second.state != "save") return false;
        size_t active = 0; for (const auto& entry : downloads) if (entry.second.file) ++active;
        if (active >= 4 || !localPath(destination)) {
            notice("Choose a new local filename; at most four downloads can run.", true); return false;
        }
        auto& d = it->second; d.file = newFile(); d.destination = destination; d.token = randomId();
        const auto temp = destination.substr(0,destination.find_last_of(L'\\')+1)+L".oi-"+wide(d.token)+L".part";
        auto file=d.file; auto token=d.token;
        d.offset=d.queuedOffset=0; d.finishing=false; d.touched=GetTickCount64();
        crypto_generichash_init(&d.hash,nullptr,0,32);
        if (!io.submit([this,file,temp,destination,key,token]() -> FileIoQueue::Completion {
            if (file->phase == 0 && GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES) {
                file->handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file->handle!=INVALID_HANDLE_VALUE)file->temporary=temp;
            }
            const bool ok=file->handle!=INVALID_HANDLE_VALUE;
            return [this,key,token,ok] {
                auto it=downloads.find(key); if(it==downloads.end()||!it->second.file||it->second.token!=token)return;
                if(!ok){failDownload(key,it->second,"cannot create download; choose a new local filename",false);return;}
                it->second.touched=GetTickCount64(); control(Accept,key,token);
            };
        })) { failDownload(key,d,"file I/O queue is busy",false); return false; }
        return true;
    }
    void receive(int peer, const vector<unsigned char>& bytes) {
        if (bytes.empty() || bytes.size() > MaxPacketBytes) return;
        NetPacket raw((const char*)bytes.data(), (int)bytes.size());
        unsigned char type; string id, token;
        if (!raw.getUInt8(type) || !raw.getString(id,32) || !validId(id) || !raw.getString(token,32)) return;
        const Key key(peer,id); const auto now = GetTickCount64();
        if (type == Offer || type == ImageOffer || type == CompressedImageOffer) {
            unsigned __int64 size; string name;
            if (!token.empty() || !raw.getUInt64(size) || size > MaxFileBytes || !raw.getString(name,200) || !validName(name) || !raw.fullyRead() || downloads.count(key)) return;
            // Bound retained metadata without evicting offers or active downloads.
            if (downloads.size() >= MaxOffers) return;
            size_t peerOffers=0;
            for(auto it=downloads.lower_bound({peer,{}});it!=downloads.end() && it->first.first==peer;++it)
                if(++peerOffers>=MaxOffersPerPeer)return;
            const bool image=type!=Offer;
            if(image) {
                if(size<=Screenshot::HeaderBytes || size>Screenshot::MaxBytes)return;
                size_t total=0,fromPeer=0;
                for(const auto& entry:downloads)if(entry.second.image){++total;if(entry.first.first==peer)++fromPeer;}
                if(total>=256 || fromPeer>=64)return;
            }
            Download d; d.image = image; d.compressed=type==CompressedImageOffer; d.name = name; d.size = size; d.touched = now; downloads.emplace(key, std::move(d)); announce(peer,id); return;
        }
        if (type == Cancel && token.empty() && raw.fullyRead()) {
            discardControls(key);
            auto it = downloads.find(key);
            if (it != downloads.end() && (it->second.file || it->second.state == "save")) {
                if(releaseDownload(it->second))it->second.state = "withdrawn";
            }
            return;
        }
        if (!validId(token)) return;
        if (type == Accept) {
            if (!raw.fullyRead() || uploads.count(key)) return;
            auto source = sources.find(id);
            if (source == sources.end() || !source->second->peers.count(peer) || now >= source->second->expires || uploads.size() >= 4) { control(Cancel,key,token); return; }
            Upload u; u.source = source->second; u.token = token; u.touched = now; crypto_generichash_init(&u.hash,nullptr,0,32);
            uploads.emplace(key,std::move(u)); return;
        }
        if (type == Ack || type == Done) {
            auto it = uploads.find(key); if (it == uploads.end() || token != it->second.token) return;
            auto& u = it->second; unsigned __int64 offset = 0;
            if (type == Ack) {
                if (!raw.getUInt64(offset) || !raw.fullyRead() || u.finishing || offset > u.offset || offset <= u.acknowledged ||
                    (offset != u.source->size && offset % ChunkBytes != 0)) return;
                u.acknowledged = offset; u.touched = now; u.progress.update(offset,now);
            } else if (raw.fullyRead() && u.finishing) {
                notice("Sent " + displayName(u.source->name) + ".",false); u.source->peers.erase(peer); uploads.erase(it);
            }
            return;
        }
        if (type == Cancel) {
            if (!raw.fullyRead()) return;
            pendingControls.erase(std::make_pair(key,token));
            auto up = uploads.find(key); if (up != uploads.end() && up->second.token == token) { notice("File transfer cancelled by recipient.",false); uploads.erase(up); }
            auto down = downloads.find(key); if (down != downloads.end() && down->second.file && down->second.token == token) failDownload(key,down->second,"sender unavailable",false);
            return;
        }
        auto it = downloads.find(key); if (it == downloads.end() || !it->second.file || it->second.token != token) return;
        auto& d = it->second;
        if (d.finishing) return;
        if (type == Chunk) {
            unsigned __int64 offset; vector<unsigned char> data;
            if (!raw.getUInt64(offset) || !raw.getBytes(data,ChunkBytes) || data.empty() || !raw.fullyRead() || offset != d.queuedOffset || data.size() > d.size-d.queuedOffset) { failDownload(key,d,"invalid chunk"); return; }
            const auto count=data.size(); auto file=d.file;
            if (!io.submit([this,file,key,token,offset,data=std::move(data)]() mutable -> FileIoQueue::Completion {
                DWORD written=0;
                bool ok=file->phase==0;
                if(ok && file->image) {
                    ok=offset<=file->image->bytes.size() && data.size()<=file->image->bytes.size()-offset;
                    if(ok)memcpy(file->image->bytes.data()+(size_t)offset,data.data(),data.size());
                } else if(ok)ok=WriteFile(file->handle,data.data(),(DWORD)data.size(),&written,nullptr) && written==data.size();
                return [this,key,token,ok,data=std::move(data)] {
                    auto it=downloads.find(key);if(it==downloads.end()||!it->second.file||it->second.token!=token)return;
                    auto& d=it->second;
                    if(!ok){failDownload(key,d,"write failed");return;}
                    crypto_generichash_update(&d.hash,data.data(),data.size());d.offset+=data.size();d.touched=GetTickCount64();d.progress.update(d.offset,d.touched);control(Ack,key,token,d.offset);
                };
            })) { failDownload(key,d,"file I/O queue is busy"); return; }
            d.queuedOffset+=count; d.touched=now;
        } else if (type == Finish) {
            unsigned char digest[32],actual[32];
            if (!raw.get(digest,32) || !raw.fullyRead() || d.offset!=d.size) { failDownload(key,d,"incomplete transfer"); return; }
            auto hash=d.hash; crypto_generichash_final(&hash,actual,32);
            if(sodium_memcmp(digest,actual,32)){failDownload(key,d,"integrity error");return;}
            if(d.image) {
                auto file=d.file;const bool compressed=d.compressed;
                if(!io.submit([this,file,key,token,compressed]() -> FileIoQueue::Completion {
                    if(file->phase!=0)return {};
                    auto image=Screenshot::expand(file->image,compressed);
                    return [this,key,token,image=std::move(image)] {
                        auto it=downloads.find(key);
                        if(it==downloads.end() || !it->second.file || it->second.token!=token)return;
                        auto& d=it->second;
                        if(!image){failDownload(key,d,"invalid image or screenshot memory exhausted");return;}
                        d.readyImage=image;d.file.reset();d.state="view";d.touched=GetTickCount64();control(Done,key,token);
                        if(viewImage)viewImage(d.readyImage);
                    };
                })) {failDownload(key,d,"file I/O queue is busy");return;}
                d.finishing=true;d.touched=now;return;
            }
            auto file=d.file;auto destination=d.destination;
            if(!io.submit([this,file,destination,key,token]() -> FileIoQueue::Completion {
                string error;
                if(file->phase!=0)return {};
                if(!FlushFileBuffers(file->handle))error="disk flush failed";
                file->close();
                if(error.empty()) {
                    HANDLE zone=CreateFileW((file->temporary+L":Zone.Identifier").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                    if(zone==INVALID_HANDLE_VALUE)error="cannot mark downloaded file; choose a drive supporting Windows security metadata";
                    else {
                        const char mark[]="[ZoneTransfer]\r\nZoneId=3\r\n";DWORD written=0;
                        bool marked=WriteFile(zone,mark,sizeof(mark)-1,&written,nullptr)&&written==sizeof(mark)-1&&FlushFileBuffers(zone);
                        bool closed=CloseHandle(zone)!=FALSE;
                        if(!marked||!closed)error="cannot write downloaded-file security metadata";
                    }
                }
                int active=0;
                if(error.empty() && file->phase.compare_exchange_strong(active,1)) {
                    // No overwrite: a destination created during download stays intact.
                    if(MoveFileExW(file->temporary.c_str(),destination.c_str(),0))file->temporary.clear();
                    else {error="cannot save destination";file->phase=0;}
                } else if(error.empty())return {};
                return [this,key,token,error] {
                    auto it=downloads.find(key);if(it==downloads.end()||!it->second.file||it->second.token!=token)return;
                    auto& d=it->second;
                    if(!error.empty()){failDownload(key,d,error);return;}
                    d.file.reset();d.state="saved";d.touched=GetTickCount64();control(Done,key,token);
                    notice("Saved "+displayName(d.name)+".",false);
                };
            })) {failDownload(key,d,"file I/O queue is busy");return;}
            d.finishing=true;d.touched=now;
        }
    }
    void update() {
        io.poll();
        const auto now = GetTickCount64();
        retryControls(now);
        for (auto it = downloads.begin(); it != downloads.end();) {
            if (it->second.file && now - it->second.touched > 30000) failDownload(it->first,it->second,"timed out");
            if (!it->second.file && now - it->second.touched > 600000) it = downloads.erase(it); else ++it;
        }
        for (auto it = uploads.begin(); it != uploads.end();) {
            if (now - it->second.touched > 30000) { control(Cancel,it->first,it->second.token); notice("File upload timed out.",true); it = uploads.erase(it); } else ++it;
        }
        // Keep progress and cancellation available until existing uploads finish.
        // Accept still rejects new downloads after the offer expires.
        for (auto it = sources.begin(); it != sources.end();) {
            const bool expired=now >= it->second->expires || it->second->peers.empty();
            if (expired && std::none_of(uploads.begin(),uploads.end(),[&](const auto& entry){return entry.first.second==it->first;})) it = sources.erase(it); else ++it;
        }
        for (auto& entry : uploads) {
            auto& u=entry.second;
            if(u.finishing||u.reading||u.cursor<u.buffer.size()||u.offset==u.source->size)continue;
            const auto key=entry.first;const auto token=u.token;auto file=u.source->file;
            const auto offset=u.offset;const unsigned count=(unsigned)(std::min)((unsigned __int64)FlightBytes,u.source->size-offset);
            u.reading=io.submit([this,file,key,token,offset,count]() -> FileIoQueue::Completion {
                vector<unsigned char> bytes(count);LARGE_INTEGER pos;pos.QuadPart=offset;DWORD read=0;
                bool ok;
                if(file->image) {
                    ok=offset<=file->image->bytes.size() && count<=file->image->bytes.size()-offset;
                    if(ok)memcpy(bytes.data(),file->image->bytes.data()+(size_t)offset,count);
                } else ok=SetFilePointerEx(file->handle,pos,nullptr,FILE_BEGIN)&&ReadFile(file->handle,bytes.data(),count,&read,nullptr)&&read==count;
                return [this,key,token,ok,bytes=std::move(bytes)]() mutable {
                    auto it=uploads.find(key);if(it==uploads.end()||it->second.token!=token)return;
                    if(!ok){control(Cancel,key,token);notice("File read failed.",true);uploads.erase(it);return;}
                    auto& u=it->second;u.reading=false;u.buffer=std::move(bytes);u.cursor=0;
                };
            });
        }
        if (uploads.empty()) return;
        unsigned __int64 outstanding = 0;
        for (const auto& entry : uploads) outstanding += entry.second.offset - entry.second.acknowledged;
        // Bound work per service tick and bytes awaiting receiver acknowledgement.
        // The receiver already accepts ordered chunks and acknowledges each offset.
        for (unsigned sent = 0; sent < FlightBytes / ChunkBytes; ++sent) {
            if (sent && GetTickCount64() - now >= 2) return;
            auto it = uploads.upper_bound(lastServed); if (it == uploads.end()) it = uploads.begin();
            const auto ready = [outstanding](const Upload& u) {
                return !u.finishing && (u.offset == u.source->size ? u.offset == u.acknowledged :
                    u.cursor < u.buffer.size() && outstanding + (std::min)((unsigned __int64)ChunkBytes,u.source->size-u.offset) <= FlightBytes);
            };
            for (size_t n = 0; n < uploads.size(); ++n) {
                if (ready(it->second)) break;
                if (++it == uploads.end()) it = uploads.begin();
            }
            auto& u = it->second; if (!ready(u)) return;
            lastServed = it->first;
            if (u.offset == u.source->size) {
                unsigned char digest[32]; auto hash = u.hash; crypto_generichash_final(&hash,digest,32);
                auto raw = header(Finish,it->first.second,u.token); raw.put(digest,32);
                u.finishing = transmit(it->first.first,raw); return;
            }
            const unsigned count = (unsigned)(std::min)((unsigned __int64)ChunkBytes,u.source->size-u.offset);
            vector<unsigned char> bytes(u.buffer.begin()+u.cursor,u.buffer.begin()+u.cursor+count);
            auto raw = header(Chunk,it->first.second,u.token); raw.putUInt64(u.offset); raw.putBytes(bytes,ChunkBytes);
            if (!transmit(it->first.first,raw)) return;
            crypto_generichash_update(&u.hash,bytes.data(),bytes.size()); u.offset += count; u.cursor += count;
            outstanding += count;
        }
    }
    void peerLeft(int peer) {
        for(auto it=pendingControls.begin();it!=pendingControls.end();)if(it->first.first.first==peer)it=pendingControls.erase(it);else ++it;
        for (auto it = uploads.begin(); it != uploads.end();) if (it->first.first == peer) it = uploads.erase(it); else ++it;
        for (auto& entry : downloads) if (entry.first.first == peer) { if(releaseDownload(entry.second))entry.second.state = "unavailable"; }
        for (auto& entry : sources) entry.second->peers.erase(peer);
    }
    void clear() { pendingControls.clear(); for(auto& entry:downloads)releaseDownload(entry.second); uploads.clear(); downloads.clear(); sources.clear(); }
};
