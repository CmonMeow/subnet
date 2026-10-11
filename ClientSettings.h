#pragma once
#include <fstream>
#include <algorithm>
#include <string>
#include <map>
#include <sstream>
#include "HostDirectoryProtocol.h"

// Save application settings without truncating the previous file.
inline bool SaveClientFile(const char* path, const std::string& bytes)
{
    unsigned char nonce[8];
    randombytes_buf(nonce, sizeof(nonce));
    char suffix[17];
    sodium_bin2hex(suffix, sizeof(suffix), nonce, sizeof(nonce));
    const std::string temporary = std::string(path) + "." + suffix + ".tmp";
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool saved = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    if (saved) saved = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) saved = false;
    if (saved) saved = MoveFileExA(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!saved) DeleteFileA(temporary.c_str());
    return saved;
}

enum ClientSettingBits
{
    CSDedicated = 1 << 1,
};

struct PackedClientSettings
{
private:
    std::map<std::string, int> _userVolumes;
    std::string _savedSettings;
    ULONGLONG _volumeSaveAt = 0;
    bool _volumesChanged = false;

    static std::string volumeName(const std::string& displayName)
    {
        const auto colon = displayName.find(':');
        std::string name = displayName.substr(colon == std::string::npos ? 0 : colon + 1);
        if (!ValidUserName(name)) return {};
        for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return name;
    }

    std::string serialized() const
    {
        std::string bytes;
        bytes += (char)(bits & CSDedicated);
        bytes += (char)talkKey;
        bytes += serverAddress + '\n' + hostName + '\n';
        for (const auto& volume : _userVolumes)
            bytes += volume.first + " " + std::to_string(volume.second) + '\n';
        return bytes;
    }

public:
    unsigned char bits = 0;
    unsigned char talkKey = VK_SHIFT;
    std::string serverAddress = DEFAULT_NETWORK_ADDRESS;
    std::string hostName = LocalUserName();

    PackedClientSettings()
    {
        load();
        _savedSettings = serialized();
    }

    void load()
    {
        std::ifstream file("ClientSettings", std::ios::binary);
        if (file.good())
        {
            unsigned char savedBits = 0;
            file.read((char*)&savedBits, sizeof(savedBits));
            if (file.gcount() == sizeof(savedBits))
            {
                bits = savedBits & CSDedicated;
                unsigned char savedKey = 0;
                if (file.read((char*)&savedKey, sizeof(savedKey)) && savedKey >= VK_BACK && savedKey != VK_ESCAPE && savedKey < 255)
                    talkKey = savedKey;
                // Optional extension after the original two settings bytes.
                // Old files keep the built-in server default.
                char address[256] = {};
                if (file.getline(address, sizeof(address)) && address[0])
                    serverAddress = address;
                char name[34]={};
                if(file.getline(name,sizeof(name))&&HostDirectory::validName(name))hostName=name;
                char entry[64];
                while (file.getline(entry, sizeof(entry)))
                {
                    std::istringstream row(entry);
                    std::string user, extra;
                    int percent;
                    if (!(row >> user >> percent) || (row >> extra) || !ValidUserName(user) || percent < 0 || percent > 200) continue;
                    if (percent == 100) _userVolumes.erase(volumeName(user));
                    else _userVolumes[volumeName(user)] = percent;
                }
            }
        }
    }

    bool save()
    {
        const auto bytes = serialized();
        if (bytes != _savedSettings && !SaveClientFile("ClientSettings", bytes)) return false;
        _savedSettings = bytes;
        _volumesChanged = false;
        _volumeSaveAt = 0;
        return true;
    }

    int userVolume(const std::string& displayName) const
    {
        const auto found = _userVolumes.find(volumeName(displayName));
        return found == _userVolumes.end() ? 100 : found->second;
    }

    void setUserVolume(const std::string& displayName, int percent)
    {
        const auto name = volumeName(displayName);
        if (name.empty()) return;
        percent = (std::max)(0, (std::min)(200, percent));
        if (userVolume(name) == percent) return;
        if (percent == 100) _userVolumes.erase(name);
        else _userVolumes[name] = percent;
        _volumesChanged = true;
        _volumeSaveAt = GetTickCount64() + 750;
    }

    bool flushVolumes(ULONGLONG now, bool closing = false)
    {
        if (!_volumesChanged || (!closing && (!_volumeSaveAt || now < _volumeSaveAt))) return true;
        _volumeSaveAt = 0; // Report failures once; retry on another edit or at shutdown.
        return save();
    }

    bool setServerAddress(const std::string& address)
    {
        if (address.empty() || address.size() > 255 || address.find_first_of("\r\n") != std::string::npos) return false;
        if (address == serverAddress) return true;
        const auto previous = serverAddress;
        serverAddress = address;
        if (save()) return true;
        serverAddress = previous;
        return false;
    }

    bool dedicated() const { return (bits & CSDedicated) != 0; }

    bool setDedicated(bool enabled)
    {
        if (dedicated() == enabled) return true;
        const auto previous = bits;
        if (enabled) bits |= CSDedicated; else bits &= ~CSDedicated;
        if (save()) return true;
        bits = previous;
        return false;
    }

    bool setHostName(const std::string& name)
    {
        if (hostName == name) return true;
        const auto previous = hostName;
        hostName = name;
        if (save()) return true;
        hostName = previous;
        return false;
    }

    bool setTalkKey(unsigned char key)
    {
        if (talkKey == key) return true;
        const auto previous = talkKey;
        talkKey = key;
        if (save()) return true;
        talkKey = previous;
        return false;
    }

    string talkKeyLabel() const
    {
        char label[64] = {};
        UINT scan = MapVirtualKeyA(talkKey, MAPVK_VK_TO_VSC);
        LONG keyData = (LONG)(scan << 16);
        if (talkKey == VK_LEFT || talkKey == VK_RIGHT || talkKey == VK_UP || talkKey == VK_DOWN ||
            talkKey == VK_HOME || talkKey == VK_END || talkKey == VK_PRIOR || talkKey == VK_NEXT ||
            talkKey == VK_INSERT || talkKey == VK_DELETE || talkKey == VK_DIVIDE || talkKey == VK_NUMLOCK)
            keyData |= 1 << 24;
        if (!GetKeyNameTextA(keyData, label, sizeof(label))) strcpy_s(label, "Key");
        string result = string("PTT:") + label;
        return result.size() > 10 ? result.substr(0, 9) + "." : result;
    }

};

