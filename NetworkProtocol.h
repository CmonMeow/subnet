#pragma once
#include "VoiceAudioFormat.h"

#include "Network/SessionTransport.hpp"
#include "sodium.h"
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

using std::string;
using std::vector;

const char* const DEFAULT_NETWORK_ADDRESS = "nigger.observer";
const unsigned short DEFAULT_NETWORK_PORT = 777;
const __int32 APP_PROTOCOL_VERSION = 32;
const char* const BAN_LIST_FILENAME = "banlist.txt";
const size_t CHAT_MAX_MESSAGE_CHARS = 1024;
const size_t CHAT_MAX_LINE_CHARS = 1152;
const size_t CHAT_WRAP_CHARS = 54;
const size_t CHAT_VISIBLE_ROWS = 9;
const size_t CHAT_MAX_HISTORY_LINES = 500;
const size_t NET_MAX_TEXT_BYTES = 4096;
const size_t NET_MAX_ENCRYPTED_BYTES = 65536;

struct NetworkIdentity
{
    __int32 protocolVersion;
    bool voiceClient;
    string driveSerial;
    string name;
};

enum ChatLineKind
{
    CLKNormal,
    CLKSystem,
    CLKPrivate,
    CLKError,
    CLKFile,
    CLKScreen,
    CLKImage
};

struct NetworkChatLine
{
    unsigned long long historyId = 0;
    SYSTEMTIME receivedAt = {};
    string text;
    std::shared_ptr<const string> message;
    size_t messageOffset = 0;
    ChatLineKind kind;
    int fileSender = -1;
    string fileId;
};

inline string SanitiseText(const string& text, size_t maxChars)
{
    string result;
    result.reserve(text.size() < maxChars ? text.size() : maxChars);
    for (size_t i = 0; i < text.size() && result.size() < maxChars; ++i)
    {
        unsigned char c = (unsigned char)text[i];
        if (c >= 32 && c != 127)
        {
            result.push_back((char)c);
        }
    }
    return result;
}

inline string SanitiseChatText(const string& text)
{
    return SanitiseText(text, CHAT_MAX_MESSAGE_CHARS);
}

inline string SanitiseChatLine(const string& text)
{
    return SanitiseText(text, CHAT_MAX_LINE_CHARS);
}

inline string TrimWhitespace(const string& text)
{
    size_t first = 0;
    while (first < text.size() && isspace((unsigned char)text[first]))
    {
        ++first;
    }
    size_t last = text.size();
    while (last > first && isspace((unsigned char)text[last - 1]))
    {
        --last;
    }
    return text.substr(first, last - first);
}

inline bool AddUniqueString(vector<string>& list, const string& value)
{
    if (value.empty() || std::find(list.begin(), list.end(), value) != list.end())
    {
        return false;
    }
    list.push_back(value);
    return true;
}

inline string SanitiseIdentityText(const string& text, size_t maxChars)
{
    string result = SanitiseText(text, maxChars);
    for (size_t i = 0; i < result.size(); ++i)
    {
        if (result[i] == '|' || result[i] == '\r' || result[i] == '\n')
        {
            result[i] = '_';
        }
    }
    return TrimWhitespace(result);
}

inline bool MatchesCommand(const string& text, const char* command)
{
    size_t length = strlen(command);
    return _strnicmp(text.c_str(), command, length) == 0 &&
           (text.size() == length || (text.size() > length && isspace((unsigned char)text[length])));
}

// The existing C: volume serial is sent only to the host for ban matching.
inline bool ValidDriveSerial(const string& serial)
{
    if (serial.empty() || serial.size() > 10 || (serial.size() > 1 && serial[0] == '0')) return false;
    if (serial.find_first_not_of("0123456789") != string::npos) return false;
    return serial.size() < 10 || serial <= "4294967295";
}

inline string CleanUserName(const string& text)
{
    string result;
    for (size_t i = 0; i < text.size() && result.size() < 32; ++i)
    {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) result.push_back((char)c);
    }
    return result.empty() ? "Anon" : result;
}

inline bool ValidUserName(const string& name)
{
    return !name.empty() && name.size() <= 32 && CleanUserName(name) == name;
}

inline string IdentityDisplayName(const string& name, __int32 netId)
{
    return std::to_string(netId) + ":" + CleanUserName(name);
}

inline string LocalDriveSerial()
{
    DWORD serial = 0;
    if (!GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0))
        return string();
    std::ostringstream value;
    value << serial;
    return value.str();
}

inline string LocalUserName()
{
    string saved;
    std::ifstream file("ClientName.txt");
    if (std::getline(file, saved) && ValidUserName(saved)) return saved;
    char buf[256];
    DWORD bufSize = sizeof(buf);
    if (GetUserNameA(buf, &bufSize) && bufSize > 1)
    {
        return CleanUserName(buf);
    }
    return "Anon";
}

inline void LoadIdentityList(const char* filename, vector<string>& list)
{
    list.clear();
    std::ifstream f(filename);
    if (!f.good())
    {
        return;
    }

    string token;
    char c;
    while (f.get(c))
    {
        if (isspace((unsigned char)c) || c == ',' || c == ';')
        {
            AddUniqueString(list, SanitiseIdentityText(token, 64));
            token.clear();
        }
        else if (token.size() < 255)
        {
            token.push_back(c);
        }
    }

    AddUniqueString(list, SanitiseIdentityText(token, 64));
}

inline void SaveIdentityList(const char* filename, const vector<string>& list)
{
    std::ofstream f(filename, std::ios::out | std::ios::trunc);
    if (!f.good())
    {
        return;
    }

    for (size_t i = 0; i < list.size(); ++i)
    {
        if (!list[i].empty())
        {
            f << list[i] << "\r\n";
        }
    }
}

inline void ReadBlockedSerials(vector<string>& list)
{
    LoadIdentityList(BAN_LIST_FILENAME, list);

}

inline void WriteBlockedSerials(const vector<string>& list)
{
    SaveIdentityList(BAN_LIST_FILENAME, list);
}

class NetPacket
{
    vector<char> _buffer;
    const char* _externalBuffer;
    __int32 _externalBufferSize;
    __int32 _pos;

public:
    NetPacket()
        : _externalBuffer(NULL), _externalBufferSize(0), _pos(0)
    {
        _buffer.reserve(512);
    }

    NetPacket(const char* buffer, __int32 size)
        : _externalBuffer(buffer), _externalBufferSize(size), _pos(0)
    {
    }

    const char* data() const
    {
        return _externalBuffer ? _externalBuffer : (_buffer.empty() ? NULL : &_buffer[0]);
    }

    __int32 size() const
    {
        return _externalBuffer ? _externalBufferSize : (__int32)_buffer.size();
    }

    void put(const void* value, __int32 valueSize)
    {
        if (!value || valueSize <= 0)
        {
            return;
        }
        __int32 minSize = _pos + valueSize;
        if ((__int32)_buffer.size() < minSize)
        {
            _buffer.resize(minSize);
        }
        memcpy(&_buffer[_pos], value, valueSize);
        _pos += valueSize;
    }

    bool get(void* value, __int32 valueSize)
    {
        const char* source = data();
        if (!value || valueSize < 0 || !source || remaining() < valueSize)
        {
            return false;
        }
        memcpy(value, source + _pos, valueSize);
        _pos += valueSize;
        return true;
    }

    __int32 remaining() const
    {
        __int32 total = size();
        if (_pos < 0 || _pos > total)
        {
            return 0;
        }
        return total - _pos;
    }

    bool fullyRead() const
    {
        return remaining() == 0;
    }

    void putUInt8(unsigned char value) { put(&value, sizeof(value)); }
    void putUInt16(unsigned short value) { put(&value, sizeof(value)); }
    void putUInt64(unsigned __int64 value) { put(&value, sizeof(value)); }
    void putUInt32(unsigned __int32 value) { put(&value, sizeof(value)); }
    void putInt32(__int32 value) { put(&value, sizeof(value)); }
    void putFloat(float value) { put(&value, sizeof(value)); }
    void putBytes(const vector<unsigned char>& value, size_t maxLength = 65535)
    {
        if (maxLength > 65535)
        {
            maxLength = 65535;
        }
        size_t length = value.size();
        if (length > maxLength)
        {
            length = maxLength;
        }
        putUInt16((unsigned short)length);
        if (length > 0)
        {
            put(&value[0], (__int32)length);
        }
    }
    void putString(const string& value, size_t maxLength = 255)
    {
        if (maxLength > 255)
        {
            maxLength = 255;
        }
        size_t length = value.size();
        if (length > maxLength)
        {
            length = maxLength;
        }
        if (length > 255)
        {
            length = 255;
        }
        putUInt8((unsigned char)length);
        if (length > 0)
        {
            put(value.data(), (__int32)length);
        }
    }
    void putLongString(const string& value, size_t maxLength) {
        const size_t length=(std::min)(value.size(),(std::min)(maxLength,size_t(65535)));
        putUInt16((unsigned short)length);
        if(length)put(value.data(),(__int32)length);
    }
    bool getLongString(string& value, size_t maxLength) {
        value.clear();unsigned short length=0;
        if(!getUInt16(length) || length>maxLength || remaining()<(__int32)length)return false;
        value.resize(length);return !length || get(&value[0],length);
    }
    bool getUInt8(unsigned char& value) { return get(&value, sizeof(value)); }
    bool getUInt16(unsigned short& value) { return get(&value, sizeof(value)); }
    bool getUInt64(unsigned __int64& value) { return get(&value, sizeof(value)); }
    bool getUInt32(unsigned __int32& value) { return get(&value, sizeof(value)); }
    bool getInt32(__int32& value) { return get(&value, sizeof(value)); }
    bool getFloat(float& value) { return get(&value, sizeof(value)); }
    bool getBytes(vector<unsigned char>& value, size_t maxLength = 65535)
    {
        value.clear();
        if (maxLength > 65535)
        {
            maxLength = 65535;
        }
        unsigned short length = 0;
        if (!getUInt16(length) || length > maxLength || remaining() < (__int32)length)
        {
            return false;
        }
        if (length == 0)
        {
            return true;
        }
        value.resize(length);
        return get(&value[0], length);
    }
    bool getString(string& value, size_t maxLength = 255)
    {
        value.clear();
        if (maxLength > 255)
        {
            maxLength = 255;
        }
        unsigned char length = 0;
        if (!getUInt8(length) || length > maxLength || remaining() < (__int32)length)
        {
            return false;
        }
        if (length == 0)
        {
            return true;
        }
        value.resize(length);
        return get(&value[0], length);
    }
};

inline string LocalExecutableName()
{
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
    if (!length || length >= sizeof(path)) return {};
    const string fullPath(path, length);
    const auto slash = fullPath.find_last_of("\\/");
    return slash == string::npos ? fullPath : fullPath.substr(slash + 1);
}

inline void EncodeLocalIdentityRaw(NetPacket& raw)
{
    raw.putInt32(APP_PROTOCOL_VERSION);
    raw.putString(LocalDriveSerial(), 10);
    raw.putString(LocalUserName(), 48);
    raw.putUInt8(_stricmp(LocalExecutableName().c_str(), "oi.exe") == 0 ? 1 : 0);
}

inline string BuildAppRawMessage(ChatPacketKind type, const NetPacket& raw)
{
    string payload;
    payload.resize(sizeof(ChatPacketPrefix) + raw.size());
    ChatPacketPrefix header;
    header.type = type;
    memcpy(&payload[0], &header, sizeof(header));
    if (raw.size() > 0)
    {
        memcpy(&payload[sizeof(header)], raw.data(), raw.size());
    }
    return payload;
}

inline bool ValidAppMessageType(ChatPacketKind type)
{
    switch (type) {
    case NAMTConnect: case NAMTDisconnect: case NAMTChat: case NAMTHeartbeat:
    case NAMTSessionEnd: case NAMTPlayerAssign: case NAMTVoice: case NAMTKeyHello: case NAMTKeyAccept:
    case NAMTScreen: case NAMTFile: case NAMTSystemNotice: case NAMTCommandError: case NAMTPresence: case NAMTChatKey: case NAMTPrivateChat: case NAMTEncrypted: return true;
    default: return false;
    }
}

inline const char* ConnectResultName(JoinResult result)
{
    switch (result)
    {
    case JoinOK: return "Sucess";
    case JoinPassword: return "Invalid Password";
    case JoinVersion: return "Incompatible Version";
    case JoinError: return "Error";
    case JoinName: return "Bad Name";
    case JoinSessionFull: return "Session Full";
    case JoinNone: return "Connecting";
    case JoinTimeout: return "Timeout";
    default: return "Unknown";
    }
}

inline bool ParseAppRawString(const char* buffer, __int32 bufferSize, ChatPacketKind expectedType, string& text, size_t maxLength)
{
    text.clear();
    if (!buffer || bufferSize < (__int32)sizeof(ChatPacketPrefix))
    {
        return false;
    }

    const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
    if (header->type != expectedType || !ValidAppMessageType(header->type))
    {
        return false;
    }

    const __int32 payloadSize = bufferSize - (__int32)sizeof(ChatPacketPrefix);
    if (payloadSize < 0 || (size_t)payloadSize > NET_MAX_TEXT_BYTES)
    {
        return false;
    }

    NetPacket raw(buffer + sizeof(ChatPacketPrefix), payloadSize);
    return raw.getLongString(text, maxLength) && raw.fullyRead();
}

inline bool ParseAppRawBytes(const char* buffer, __int32 bufferSize, ChatPacketKind expectedType, string& bytes, size_t expectedBytes)
{
    bytes.clear();
    if (!buffer || bufferSize < (__int32)sizeof(ChatPacketPrefix))
    {
        return false;
    }

    const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
    if (header->type != expectedType || !ValidAppMessageType(header->type))
    {
        return false;
    }

    const __int32 payloadSize = bufferSize - (__int32)sizeof(ChatPacketPrefix);
    if (payloadSize < 0 || (size_t)payloadSize != expectedBytes || expectedBytes > NET_MAX_ENCRYPTED_BYTES)
    {
        return false;
    }

    bytes.assign(buffer + sizeof(ChatPacketPrefix), buffer + bufferSize);
    return true;
}

inline bool ParseAppRawControl(const char* buffer, __int32 bufferSize, ChatPacketKind expectedType)
{
    if (!buffer || bufferSize != (__int32)sizeof(ChatPacketPrefix))
    {
        return false;
    }

    const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
    return header->type == expectedType && ValidAppMessageType(header->type);
}

inline bool ParseIdentityRaw(const char* buffer, __int32 bufferSize, NetworkIdentity& identity)
{
    identity = NetworkIdentity();
    if (!buffer || bufferSize < (__int32)sizeof(ChatPacketPrefix))
    {
        return false;
    }

    const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
    if (header->type != NAMTConnect || !ValidAppMessageType(header->type))
    {
        return false;
    }

    const __int32 payloadSize = bufferSize - (__int32)sizeof(ChatPacketPrefix);
    if (payloadSize < 0 || (size_t)payloadSize > NET_MAX_TEXT_BYTES)
    {
        return false;
    }

    NetPacket raw(buffer + sizeof(ChatPacketPrefix), payloadSize);
    __int32 version = 0;
    string driveSerial;
    string name;
    unsigned char voiceClient = 0;
    if (!raw.getInt32(version) ||
        !raw.getString(driveSerial, 10) ||
        !raw.getString(name, 48))
    {
        return false;
    }
    if (raw.remaining() > 0 && !raw.getUInt8(voiceClient))
    {
        return false;
    }
    if (!raw.fullyRead())
    {
        return false;
    }
    if (version != APP_PROTOCOL_VERSION)
    {
        return false;
    }

    identity.protocolVersion = version;
    identity.voiceClient = voiceClient != 0;
    identity.driveSerial = driveSerial;
    identity.name = SanitiseIdentityText(name, 48);
    if (identity.name.empty())
    {
        identity.name = "Anon";
    }
    return ValidDriveSerial(identity.driveSerial) && ValidUserName(identity.name);
}

#pragma pack(push, networkPlayerPackets, 1)
struct NetworkPlayerAssignPacket
{
    __int32 playerId;
};

enum { VOICE_SAMPLE_RATE = VoiceAudioFormat::SampleRate, VOICE_SAMPLES_PER_PACKET = VoiceAudioFormat::FrameSamples,
       VOICE_MAX_OPUS_BYTES = 1275, VOICE_CODEC_OPUS = 1, VOICE_CODEC_ADPCM = 2 };
#pragma pack(pop, networkPlayerPackets)

struct NetworkVoicePacket
{
    __int32 playerId;
    unsigned __int32 sequence;
    unsigned char codec;
    unsigned short sampleRate;
    unsigned short frameSamples;
    vector<unsigned char> data;
};

inline void EncodeAppPacketRaw(NetPacket& raw, const NetworkPlayerAssignPacket& packet)
{
    raw.putInt32(packet.playerId);
}

inline bool DecodeAppPacketRaw(NetPacket& raw, NetworkPlayerAssignPacket& packet)
{
    return raw.getInt32(packet.playerId) && raw.fullyRead();
}

template<class T>
inline string BuildAppPacket(ChatPacketKind type, const T& packet)
{
    NetPacket raw;
    EncodeAppPacketRaw(raw, packet);
    return BuildAppRawMessage(type, raw);
}

template<class T>
inline bool ParseAppPacket(const char* buffer, __int32 bufferSize, ChatPacketKind expectedType, T& packet)
{
    if (!buffer || bufferSize < (__int32)sizeof(ChatPacketPrefix))
    {
        return false;
    }
    const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
    if (header->type != expectedType || !ValidAppMessageType(header->type))
    {
        return false;
    }
    const __int32 payloadSize = bufferSize - (__int32)sizeof(ChatPacketPrefix);
    if (payloadSize < 0 || (size_t)payloadSize > NET_MAX_ENCRYPTED_BYTES)
    {
        return false;
    }
    NetPacket raw(buffer + sizeof(ChatPacketPrefix), payloadSize);
    return DecodeAppPacketRaw(raw, packet);
}

class cCryptoSession
{
    static constexpr size_t ReplayWindow = 65536;
    uint64_t _sendSequence = 0;
    uint64_t _receiveSequence = 0;
    std::bitset<ReplayWindow> _received;
    bool _hasKeypair;
    bool _ready;
    unsigned char _publicKey[crypto_kx_PUBLICKEYBYTES];
    unsigned char _secretKey[crypto_kx_SECRETKEYBYTES];
    unsigned char _rx[crypto_kx_SESSIONKEYBYTES];
    unsigned char _tx[crypto_kx_SESSIONKEYBYTES];

public:
    cCryptoSession()
        : _hasKeypair(false),
          _ready(false)
    {
        memset(_publicKey, 0, sizeof(_publicKey));
        memset(_secretKey, 0, sizeof(_secretKey));
        memset(_rx, 0, sizeof(_rx));
        memset(_tx, 0, sizeof(_tx));
    }

    ~cCryptoSession() { clear(); }

    bool ready() const { return _ready; }
    const unsigned char* receiveKey() const { return _rx; }
    const unsigned char* sendKey() const { return _tx; }

    void clear()
    {
        _sendSequence = 0;
        _receiveSequence = 0;
        _received.reset();
        _hasKeypair = false;
        _ready = false;
        sodium_memzero(_publicKey, sizeof(_publicKey));
        sodium_memzero(_secretKey, sizeof(_secretKey));
        sodium_memzero(_rx, sizeof(_rx));
        sodium_memzero(_tx, sizeof(_tx));
    }

    bool buildClientHello(string& payload)
    {
        clear();
        if (crypto_kx_keypair(_publicKey, _secretKey) != 0)
        {
            return false;
        }
        _hasKeypair = true;
        _ready = false;
        payload.assign(reinterpret_cast<const char*>(_publicKey), sizeof(_publicKey));
        return true;
    }

    bool acceptServerKey(const string& payload)
    {
        if (_ready || !_hasKeypair || payload.size() != crypto_kx_PUBLICKEYBYTES)
        {
            return false;
        }
        const unsigned char* serverPublic = reinterpret_cast<const unsigned char*>(payload.data());
        if (crypto_kx_client_session_keys(_rx, _tx, _publicKey, _secretKey, serverPublic) != 0)
        {
            clear();
            return false;
        }
        _ready = true;
        return true;
    }

    bool acceptClientHello(const string& payload, string& response)
    {
        clear();
        if (payload.size() != crypto_kx_PUBLICKEYBYTES)
        {
            return false;
        }
        if (crypto_kx_keypair(_publicKey, _secretKey) != 0)
        {
            return false;
        }
        _hasKeypair = true;
        const unsigned char* clientPublic = reinterpret_cast<const unsigned char*>(payload.data());
        if (crypto_kx_server_session_keys(_rx, _tx, _publicKey, _secretKey, clientPublic) != 0)
        {
            clear();
            return false;
        }
        _ready = true;
        response.assign(reinterpret_cast<const char*>(_publicKey), sizeof(_publicKey));
        return true;
    }

    bool encrypt(const string& plain, string& payload)
    {
        if (!_ready || _sendSequence == UINT64_MAX)
        {
            return false;
        }
        unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
        randombytes_buf(nonce, sizeof(nonce));
        const uint64_t sequence = ++_sendSequence;
        for (unsigned i = 0; i < sizeof(sequence); ++i)
            nonce[sizeof(nonce) - sizeof(sequence) + i] = (unsigned char)(sequence >> (i * 8));

        payload.resize(sizeof(ChatPacketPrefix) + sizeof(nonce) + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
        ChatPacketPrefix header;
        header.type = NAMTEncrypted;
        memcpy(&payload[0], &header, sizeof(header));
        memcpy(&payload[sizeof(header)], nonce, sizeof(nonce));

        unsigned long long cipherBytes = 0;
        if (crypto_aead_xchacha20poly1305_ietf_encrypt(
                reinterpret_cast<unsigned char*>(&payload[sizeof(header) + sizeof(nonce)]), &cipherBytes,
                reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
                NULL, 0, NULL, nonce, _tx) != 0)
        {
            payload.clear();
            return false;
        }
        payload.resize(sizeof(header) + sizeof(nonce) + (size_t)cipherBytes);
        return true;
    }

    bool decrypt(const char* buffer, __int32 bufferSize, string& plain)
    {
        if (!_ready || !buffer ||
            bufferSize < (__int32)(sizeof(ChatPacketPrefix) + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES) ||
            (size_t)bufferSize > NET_MAX_ENCRYPTED_BYTES)
        {
            return false;
        }
        const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
        if (header->type != NAMTEncrypted)
        {
            return false;
        }
        const unsigned char* nonce = reinterpret_cast<const unsigned char*>(buffer + sizeof(ChatPacketPrefix));
        uint64_t sequence = 0;
        for (unsigned i = 0; i < sizeof(sequence); ++i)
            sequence |= uint64_t(nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES - sizeof(sequence) + i]) << (i * 8);
        if (!sequence || (sequence <= _receiveSequence &&
            (_receiveSequence - sequence >= ReplayWindow || _received[(size_t)(sequence % ReplayWindow)])))
        {
            plain.clear();
            return false;
        }
        const unsigned char* cipher = reinterpret_cast<const unsigned char*>(buffer + sizeof(ChatPacketPrefix) + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
        const __int32 cipherSize = bufferSize - (__int32)(sizeof(ChatPacketPrefix) + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
        plain.resize(cipherSize);

        unsigned long long plainBytes = 0;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                reinterpret_cast<unsigned char*>(&plain[0]), &plainBytes, NULL,
                cipher, cipherSize, NULL, 0, nonce, _rx) != 0)
        {
            plain.clear();
            return false;
        }
        // Commit only after authentication. Forged counters must not evict valid packets.
        if (sequence > _receiveSequence)
        {
            const uint64_t advance = sequence - _receiveSequence;
            if (advance >= ReplayWindow) _received.reset();
            else for (uint64_t step = 1; step <= advance; ++step)
                _received.reset((size_t)((_receiveSequence + step) % ReplayWindow));
            _receiveSequence = sequence;
        }
        _received.set((size_t)(sequence % ReplayWindow));
        plain.resize((size_t)plainBytes);
        return true;
    }
};

inline string BuildVoicePayload(const NetworkVoicePacket& packet)
{
    NetPacket raw;
    raw.putInt32(packet.playerId);
    raw.putUInt32(packet.sequence);
    raw.putUInt8(packet.codec);
    raw.putUInt16(packet.sampleRate);
    raw.putUInt16(packet.frameSamples);
    raw.putBytes(packet.data, VOICE_MAX_OPUS_BYTES);
    return BuildAppRawMessage(NAMTVoice, raw);
}

inline bool ParseVoicePacket(const char* buffer, __int32 bufferSize, NetworkVoicePacket& packet)
{
    packet = NetworkVoicePacket();
    if (!buffer || bufferSize < (__int32)sizeof(ChatPacketPrefix))
    {
        return false;
    }
    const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
    if (header->type != NAMTVoice)
    {
        return false;
    }

    const __int32 payloadSize = bufferSize - (__int32)sizeof(ChatPacketPrefix);
    if (payloadSize < 0 || (size_t)payloadSize > NET_MAX_ENCRYPTED_BYTES)
    {
        return false;
    }

    NetPacket raw(buffer + sizeof(ChatPacketPrefix), payloadSize);
    return raw.getInt32(packet.playerId) &&
           raw.getUInt32(packet.sequence) &&
           raw.getUInt8(packet.codec) &&
           raw.getUInt16(packet.sampleRate) &&
           raw.getUInt16(packet.frameSamples) &&
           raw.getBytes(packet.data, VOICE_MAX_OPUS_BYTES) &&
           raw.fullyRead();
}
