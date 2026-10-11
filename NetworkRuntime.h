#pragma once
#include "ClientSettings.h"
#include "ChatCommands.h"
#include "FileTransfers.h"
#include "FileSaveDialog.h"
#include "ImageViewer.h"
#include "ScreenSignaling.h"

class cNetworkRuntime
{
    ULONGLONG _disconnectDeadline=0;
    struct EndingPeer { ULONGLONG deadline; DisconnectReason reason; string text; };
    std::map<int,EndingPeer> _endingPeers;
    string _hostName;
    unsigned short _hostingPort = DEFAULT_NETWORK_PORT;
    ScreenSignaling _screens;
    FileTransfers _files;
    ULONGLONG _voiceActiveUntil = 0;
    FileSaveDialog _fileDialog;
    ImageViewer _imageViewer;
    struct FileBudget { double bytes = 32768; double packets = 32; ULONGLONG time = GetTickCount64(); };
    struct RelayedFile { int from; vector<unsigned char> cipher; };
    std::map<int, std::deque<RelayedFile>> _fileRelay;
    size_t _fileRelayBytes = 0;
    int _lastFileRelay = -1;
    void clearFileRelay(int peer) {
        for(auto it = _fileRelay.begin(); it != _fileRelay.end();) {
            auto& queue = it->second;
            const auto removed = std::remove_if(queue.begin(), queue.end(), [&](const RelayedFile& packet) {
                if(it->first != peer && packet.from != peer) return false;
                _fileRelayBytes -= packet.cipher.size();
                return true;
            });
            queue.erase(removed, queue.end());
            if(queue.empty()) it = _fileRelay.erase(it); else ++it;
        }
    }
    void flushFileRelay() {
        const auto started = GetTickCount64();
        for(unsigned work = 0; work < 256 && !_fileRelay.empty(); ++work) {
            auto it = _fileRelay.upper_bound(_lastFileRelay);
            if(it == _fileRelay.end()) it = _fileRelay.begin();
            _lastFileRelay = it->first;
            auto& packet = it->second.front();
            if(fileQueueAvailable(it->first, packet.cipher.size() <= 552)) {
                NetPacket raw;
                raw.putInt32(packet.from);
                raw.putBytes(packet.cipher, FileTransfers::MaxPacketBytes + 40);
                if(sendRawFromServer(it->first, NAMTFile, raw, DeliveryGuaranteed)) {
                    _fileRelayBytes -= packet.cipher.size();
                    it->second.pop_front();
                    if(it->second.empty()) _fileRelay.erase(it);
                }
            }
            if(GetTickCount64() - started >= 2) break;
        }
    }
    struct PeerBudget { FileBudget voice, control; };
    std::map<int, PeerBudget> _peerBudgets;
    bool fileBudget(FileBudget& budget, size_t bytes, double rate, double packetRate = 4096) {
        const auto now = GetTickCount64(); const double elapsed = (now-budget.time)/1000.0; budget.time = now;
        budget.bytes = (std::min)(rate, budget.bytes + elapsed*rate);
        budget.packets = (std::min)(packetRate*2, budget.packets + elapsed*packetRate);
        if (budget.bytes < bytes || budget.packets < 1) return false;
        budget.bytes -= bytes; budget.packets -= 1; return true;
    }
    bool fileQueueAvailable(int to, bool control = false) {
        int messages=0, bytes=0, guaranteedMessages=0, guaranteedBytes=0;
        if (_server) _server->QueryPendingSends(to,messages,bytes,guaranteedMessages,guaranteedBytes);
        else if (_client) _client->QueryPendingSends(messages,bytes,guaranteedMessages,guaranteedBytes);
        else return false;
        // Keep bounded headroom for offers/accepts/acks during screen negotiation bursts.
        // Four downloads can each have a 1 MiB application window in flight.
        // Allow framing/retransmission headroom, plus reserved space for controls.
        const int maxBytes=control?9437184:8388608, maxMessages=control?18432:16384;
        return bytes >= 0 && guaranteedBytes >= 0 && bytes < maxBytes && guaranteedBytes < maxBytes &&
            messages < maxMessages && guaranteedMessages < maxMessages;
    }
    bool sendFilePayload(int to, const vector<unsigned char>& plain) {
        auto key = _privateChatKeys.find(to);
        if ((!isHost() && !clientReady()) || to == localPlayerId() || key == _privateChatKeys.end() ||
            key->second.size() != crypto_box_PUBLICKEYBYTES || !fileQueueAvailable(to,plain.size()<=512) || plain.empty() || plain.size() > FileTransfers::MaxPacketBytes) return false;
        vector<unsigned char> cipher(crypto_box_NONCEBYTES + crypto_box_MACBYTES + plain.size());
        randombytes_buf(cipher.data(),crypto_box_NONCEBYTES);
        if (crypto_box_easy(cipher.data()+crypto_box_NONCEBYTES,plain.data(),plain.size(),cipher.data(),
            (const unsigned char*)key->second.data(),_privateChatSecretKey) != 0) return false;
        NetPacket raw; raw.putInt32(isHost() ? 0 : to); raw.putBytes(cipher,FileTransfers::MaxPacketBytes+40);
        return isHost() ? sendRawFromServer(to,NAMTFile,raw,DeliveryGuaranteed) :
            sendRawFromClient(NAMTFile,raw,DeliveryGuaranteed);
    }
    void receiveFilePayload(int from, const vector<unsigned char>& cipher) {
        auto key = _privateChatKeys.find(from);
        if (from == localPlayerId() || key == _privateChatKeys.end() || key->second.size() != crypto_box_PUBLICKEYBYTES ||
            cipher.size() <= crypto_box_NONCEBYTES + crypto_box_MACBYTES) return;
        vector<unsigned char> plain(cipher.size()-crypto_box_NONCEBYTES-crypto_box_MACBYTES);
        if (crypto_box_open_easy(plain.data(),cipher.data()+crypto_box_NONCEBYTES,cipher.size()-crypto_box_NONCEBYTES,
            cipher.data(),(const unsigned char*)key->second.data(),_privateChatSecretKey) != 0) return;
        _files.receive(from,plain);
    }
    void handleFileMessage(int from, const char* message, int length, bool relay) {
        NetPacket raw(message+sizeof(ChatPacketPrefix),length-sizeof(ChatPacketPrefix));
        int peer; vector<unsigned char> cipher;
        if (!raw.getInt32(peer) || peer < 0 || !raw.getBytes(cipher,FileTransfers::MaxPacketBytes+40) || !raw.fullyRead() || cipher.size() <= 40) return;
        const int sender = relay ? from : peer;
        if (!_privateChatKeys.count(sender)) return;
        if (relay && peer != 0) {
            if (peer == from || !_playerIdentities.count(peer)) return;
            NetPacket forwarded;
            forwarded.putInt32(from);
            forwarded.putBytes(cipher,FileTransfers::MaxPacketBytes+40);
            if (!_fileRelay.count(peer) && fileQueueAvailable(peer,cipher.size()<=552) &&
                sendRawFromServer(peer,NAMTFile,forwarded,DeliveryGuaranteed)) return;
            // Queue congestion, not a bandwidth cap. Never silently acknowledge
            // data we cannot retain: retire the sender explicitly on exhaustion.
            if (_fileRelayBytes + cipher.size() > 64 * 1024 * 1024) {
                endPlayerSession(from, DisconnectOther, "File relay backlog exhausted. Reconnect and retry.");
                return;
            }
            _fileRelayBytes += cipher.size();
            _fileRelay[peer].push_back({from,std::move(cipher)});
        } else receiveFilePayload(sender,cipher);
    }
    void announceFile(int from, const string& id) {
        const bool image=_files.isImage({from,id});
        addChatLine((from < 0 ? string("You") : playerDisplayName(from)) + (image ? " offered an image:" : " offered a file:"),CLKSystem);
        NetworkChatLine line; line.kind = image ? CLKImage : CLKFile; line.fileSender = from; line.fileId = id;
        line.historyId = ++_nextChatLineId; GetLocalTime(&line.receivedAt);
        _chatLines.push_back(line);
        while (_chatLines.size() > CHAT_MAX_HISTORY_LINES) _chatLines.erase(_chatLines.begin());
    }
    bool _remoteEnded = false;
    unsigned __int32 _rosterRevision = 0;
    HWND _hWnd;
    PackedClientSettings* _settings;
    mutable std::vector<std::pair<__int32, string>> _participantCache;
    mutable bool _participantsDirty = true;
    string _connectingAddress;
    bool _transportConnecting = false;
    unsigned __int64 _connectDeadline = 0;
    HostSessionInterface* _server;
    ClientSessionInterface* _client;
    vector<__int32> _players;
    vector<NetworkChatLine> _chatLines;
    unsigned long long _nextChatLineId = 0;
    unsigned __int64 _nextClientHeartbeat;
    unsigned __int64 _nextServerBroadcast;
    unsigned __int64 _nextHostStatistics = 0;
    __int32 _localPlayerId;
    __int32 _latencyMS;
    __int32 _throughputBPS;
    vector<string> _bannedSerials;
    std::map<__int32, NetworkIdentity> _playerIdentities;
    std::map<__int32, string> _privateChatKeys;
    std::map<__int32, string> _privateChatNames;
    std::map<__int32, string> _pendingLeaveMessages;
    cCryptoSession _clientCrypto;
    std::map<__int32, cCryptoSession> _serverCrypto;
    unsigned char _privateChatPublicKey[crypto_box_PUBLICKEYBYTES];
    unsigned char _privateChatSecretKey[crypto_box_SECRETKEYBYTES];
    bool _privateChatKeyReady;
    std::map<__int32, std::deque<NetworkVoicePacket>> _voicePackets;

    void queueVoicePacket(const NetworkVoicePacket& packet)
    {
        auto found = _voicePackets.find(packet.playerId);
        if (found == _voicePackets.end())
        {
            if (_voicePackets.size() >= 128) return;
            found = _voicePackets.emplace(packet.playerId, std::deque<NetworkVoicePacket>()).first;
        }
        // Match the mixer's 240 ms bound per speaker. A busy room must not
        // overflow a single shared queue before the mixer gets its next turn.
        auto& packets = found->second;
        if (packets.size() >= 12) packets.pop_front();
        packets.push_back(packet);
    }

    string privateChatPublicKeyBytes() const
    {
        return string(reinterpret_cast<const char*>(_privateChatPublicKey), crypto_box_PUBLICKEYBYTES);
    }

    bool ensurePrivateChatKey()
    {
        if (_privateChatKeyReady)
        {
            return true;
        }
        if (crypto_box_keypair(_privateChatPublicKey, _privateChatSecretKey) != 0)
        {
            return false;
        }
        _privateChatKeyReady = true;
        _privateChatKeys[0] = privateChatPublicKeyBytes();
        _privateChatNames[0] = _hostName;
        return true;
    }

    void encodeChatKey(NetPacket& raw, __int32 player, const string& name, const string& publicKey) const
    {
        raw.putInt32(player);
        raw.putString(name, 48);
        raw.putString(publicKey, crypto_box_PUBLICKEYBYTES);
    }

    bool decodeChatKey(const char* message, __int32 messageSize, __int32& player, string& name, string& publicKey)
    {
        if (!message || messageSize < (__int32)sizeof(ChatPacketPrefix))
        {
            return false;
        }
        const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(message);
        if (header->type != NAMTChatKey)
        {
            return false;
        }
        NetPacket raw(message + sizeof(ChatPacketPrefix), messageSize - (__int32)sizeof(ChatPacketPrefix));
        return raw.getInt32(player) &&
               raw.getString(name, 48) &&
               raw.getString(publicKey, crypto_box_PUBLICKEYBYTES) &&
               publicKey.size() == crypto_box_PUBLICKEYBYTES &&
               raw.fullyRead();
    }

    void sendChatKeyTo(__int32 to, __int32 player, const string& name, const string& publicKey)
    {
        if (!_server)
        {
            return;
        }
        NetPacket raw;
        encodeChatKey(raw, player, name, publicKey);
        sendRawFromServer(to, NAMTChatKey, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void broadcastChatKey(__int32 player, const string& name, const string& publicKey)
    {
        for (size_t i = 0; i < _players.size(); ++i)
        {
            sendChatKeyTo(_players[i], player, name, publicKey);
        }
    }

    void sendKnownChatKeysTo(__int32 to)
    {
        for (std::map<__int32, string>::const_iterator i = _privateChatKeys.begin(); i != _privateChatKeys.end(); ++i)
        {
            std::map<__int32, string>::const_iterator name = _privateChatNames.find(i->first);
            sendChatKeyTo(to, i->first, name != _privateChatNames.end() ? name->second : playerDisplayName(i->first), i->second);
        }
    }

    bool encryptPrivateText(__int32 target, const string& message, string& cipher)
    {
        std::map<__int32, string>::const_iterator key = _privateChatKeys.find(target);
        if (key == _privateChatKeys.end() || key->second.size() != crypto_box_PUBLICKEYBYTES)
        {
            return false;
        }
        cipher.resize(message.size() + crypto_box_SEALBYTES);
        if (crypto_box_seal(reinterpret_cast<unsigned char*>(&cipher[0]),
                            reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                            reinterpret_cast<const unsigned char*>(key->second.data())) != 0)
        {
            cipher.clear();
            return false;
        }
        return true;
    }

    bool decryptPrivateText(const string& cipher, string& message)
    {
        message.clear();
        if (!_privateChatKeyReady || cipher.size() < crypto_box_SEALBYTES)
        {
            return false;
        }
        message.resize(cipher.size() - crypto_box_SEALBYTES);
        if (crypto_box_seal_open(reinterpret_cast<unsigned char*>(&message[0]),
                                 reinterpret_cast<const unsigned char*>(cipher.data()), cipher.size(),
                                 _privateChatPublicKey, _privateChatSecretKey) != 0)
        {
            message.clear();
            return false;
        }
        message = SanitiseChatText(message);
        return !message.empty();
    }

    bool decodePrivateChatForServer(const char* message, __int32 messageSize, __int32& target, string& cipher)
    {
        if (!message || messageSize < (__int32)sizeof(ChatPacketPrefix))
        {
            return false;
        }
        const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(message);
        if (header->type != NAMTPrivateChat)
        {
            return false;
        }
        NetPacket raw(message + sizeof(ChatPacketPrefix), messageSize - (__int32)sizeof(ChatPacketPrefix));
        return raw.getInt32(target) && raw.getString(cipher, 255) && raw.fullyRead();
    }

    bool decodePrivateChatForClient(const char* message, __int32 messageSize, __int32& from, string& name, string& cipher)
    {
        if (!message || messageSize < (__int32)sizeof(ChatPacketPrefix))
        {
            return false;
        }
        const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(message);
        if (header->type != NAMTPrivateChat)
        {
            return false;
        }
        NetPacket raw(message + sizeof(ChatPacketPrefix), messageSize - (__int32)sizeof(ChatPacketPrefix));
        return raw.getInt32(from) && raw.getString(name, 48) && raw.getString(cipher, 255) && raw.fullyRead();
    }

    void sendPrivateChatToClient(__int32 to, __int32 from, const string& fromName, const string& cipher)
    {
        NetPacket raw;
        raw.putInt32(from);
        raw.putString(fromName, 48);
        raw.putString(cipher, 255);
        sendRawFromServer(to, NAMTPrivateChat, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    static void OnClientMessage(char* buffer, __int32 bufferSize, void* context)
    {
        static_cast<cNetworkRuntime*>(context)->onClientMessage(buffer, bufferSize);
    }

    static void OnServerMessage(__int32 from, char* buffer, __int32 bufferSize, void* context)
    {
        static_cast<cNetworkRuntime*>(context)->onServerMessage(from, buffer, bufferSize);
    }

    static void OnPeerJoined(__int32 player, bool, const char* name, unsigned long, void* context)
    {
        static_cast<cNetworkRuntime*>(context)->onCreatePlayer(player, name);
    }

    static void OnPeerLeft(__int32 player, void* context)
    {
        static_cast<cNetworkRuntime*>(context)->onDeletePlayer(player);
    }

    bool prepareClientMessage(char* buffer, __int32 bufferSize, string& decrypted, const char*& message, __int32& messageSize)
    {
        message = buffer;
        messageSize = bufferSize;
        if (!buffer ||
            bufferSize < (__int32)sizeof(ChatPacketPrefix) ||
            (size_t)bufferSize > NET_MAX_ENCRYPTED_BYTES)
        {
            return false;
        }
        const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
        if (!ValidAppMessageType(header->type))
        {
            return false;
        }
        if (header->type == NAMTEncrypted)
        {
            if (!_clientCrypto.decrypt(buffer, bufferSize, decrypted))
            {
                return false;
            }
            message = decrypted.data();
            messageSize = (__int32)decrypted.size();
            return true;
        }
        return !_clientCrypto.ready() && header->type == NAMTKeyAccept;
    }

    bool prepareServerMessage(__int32 from, char* buffer, __int32 bufferSize, string& decrypted, const char*& message, __int32& messageSize)
    {
        message = buffer;
        messageSize = bufferSize;
        if (!buffer ||
            bufferSize < (__int32)sizeof(ChatPacketPrefix) ||
            (size_t)bufferSize > NET_MAX_ENCRYPTED_BYTES)
        {
            return false;
        }
        const ChatPacketPrefix* header = reinterpret_cast<const ChatPacketPrefix*>(buffer);
        if (!ValidAppMessageType(header->type))
        {
            return false;
        }
        if (header->type == NAMTEncrypted)
        {
            std::map<__int32, cCryptoSession>::iterator found = _serverCrypto.find(from);
            if (found == _serverCrypto.end() || !found->second.decrypt(buffer, bufferSize, decrypted))
            {
                return false;
            }
            message = decrypted.data();
            messageSize = (__int32)decrypted.size();
            return true;
        }
        const auto session = _serverCrypto.find(from);
        return header->type == NAMTKeyHello &&
            (session == _serverCrypto.end() || !session->second.ready());
    }

    void sendPlainRawFromClient(ChatPacketKind type, const NetPacket& raw)
    {
        if (!_client)
        {
            return;
        }
        string payload = BuildAppRawMessage(type, raw);
        DWORD msgID = 0;
        _client->SendPayload((BYTE*)payload.data(), (__int32)payload.size(), msgID, DeliveryGuaranteed | DeliveryHighPriority, IntrusivePtr<PacketBuffer>());
    }

    void sendPlainRawFromServer(__int32 player, ChatPacketKind type, const NetPacket& raw)
    {
        if (!_server)
        {
            return;
        }
        string payload = BuildAppRawMessage(type, raw);
        DWORD msgID = 0;
        _server->SendPayload(player, (BYTE*)payload.data(), (__int32)payload.size(), msgID, DeliveryGuaranteed | DeliveryHighPriority, IntrusivePtr<PacketBuffer>());
    }

    void beginClientCryptoHandshake()
    {
        string hello;
        if (_clientCrypto.buildClientHello(hello))
        {
            NetPacket raw;
            raw.put(hello.data(), (__int32)hello.size());
            sendPlainRawFromClient(NAMTKeyHello, raw);
        }
    }

    static bool saneVoicePacket(const NetworkVoicePacket& packet)
    {
        return (packet.codec == VOICE_CODEC_OPUS || packet.codec == VOICE_CODEC_ADPCM) &&
               packet.sampleRate == VOICE_SAMPLE_RATE &&
               packet.frameSamples == VOICE_SAMPLES_PER_PACKET &&
               !packet.data.empty() &&
               packet.data.size() <= VOICE_MAX_OPUS_BYTES;
    }

    bool outgoingPending(__int32 onlyPlayer=-1) const
    {
        __int32 count=0,bytes=0,guaranteed=0,guaranteedBytes=0;
        if(_client){
            _client->QueryPendingSends(count,bytes,guaranteed,guaranteedBytes);
            if(count>0||guaranteed>0)return true;
        }
        if(_server)for(__int32 player:_players){
            if(onlyPlayer>=0&&player!=onlyPlayer)continue;
            _server->QueryPendingSends(player,count,bytes,guaranteed,guaranteedBytes);
            if(count>0||guaranteed>0)return true;
        }
        return false;
    }
    void drainEndingPeers()
    {
        if(!_server)return;
        const auto now=GetTickCount64();
        for(auto it=_endingPeers.begin();it!=_endingPeers.end();){
            if(now<it->second.deadline&&outgoingPending(it->first)){++it;continue;}
            _server->DisconnectPeer(it->first,it->second.reason,it->second.text.c_str());
            it=_endingPeers.erase(it);
        }
    }
    void endPlayerSession(__int32 player, DisconnectReason reason, const char* text)
    {
        if(!_server||_endingPeers.count(player))return;
        auto crypto=_serverCrypto.find(player);
        if(crypto!=_serverCrypto.end()&&crypto->second.ready()){
            NetPacket raw;raw.putString(text,CHAT_MAX_LINE_CHARS);
            sendRawFromServer(player,NAMTSessionEnd,raw,(DeliveryOptions)(DeliveryGuaranteed|DeliveryHighPriority));
            _endingPeers.emplace(player,EndingPeer{GetTickCount64()+250,reason,text});
        }else _server->DisconnectPeer(player,reason,text);
    }

    void sendPresenceTo(__int32 player)
    {
        NetPacket raw;
        raw.putInt32(_rosterRevision);
        raw.putInt32(static_cast<__int32>(_playerIdentities.size() + 1));
        raw.putInt32(0);
        raw.putString(_hostName, 48);
        for (const auto& entry : _playerIdentities)
        {
            raw.putInt32(entry.first);
            raw.putString(entry.second.name, 48);
        }
        sendRawFromServer(player, NAMTPresence, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void broadcastPresence()
    {
        _participantsDirty = true;
        ++_rosterRevision;
        for (const auto& entry : _playerIdentities) sendPresenceTo(entry.first);
    }

    void onClientMessage(char* buffer, __int32 bufferSize)
    {
        string decrypted;
        const char* message = NULL;
        __int32 messageSize = 0;
        if (!prepareClientMessage(buffer, bufferSize, decrypted, message, messageSize))
        {
            return;
        }

        NetworkPlayerAssignPacket assignPacket;
        if (ParseAppPacket(message, messageSize, NAMTPlayerAssign, assignPacket))
        {
            _localPlayerId = assignPacket.playerId;
            _participantsDirty = true;
            if (_localPlayerId >= 0) _connectDeadline = 0;
            if (_settings && _localPlayerId >= 0 && !_settings->setServerAddress(_connectingAddress))
                showNotice("Connected; could not save server setting.", true);

            return;
        }

        if (messageSize < sizeof(ChatPacketPrefix)) return;
        if (reinterpret_cast<const ChatPacketPrefix*>(message)->type == NAMTScreen) { NetPacket raw(message+sizeof(ChatPacketPrefix),messageSize-sizeof(ChatPacketPrefix)); _screens.receive(0,raw); return; }
        if (reinterpret_cast<const ChatPacketPrefix*>(message)->type == NAMTFile) { handleFileMessage(0,message,messageSize,false); return; }
        if (reinterpret_cast<const ChatPacketPrefix*>(message)->type == NAMTPresence)
        {
            NetPacket raw(message + sizeof(ChatPacketPrefix), messageSize - sizeof(ChatPacketPrefix));
            __int32 revision, count;
            if (!raw.getInt32(revision) || !raw.getInt32(count) || count < 1 || count > 256) return;
            std::map<__int32, NetworkIdentity> roster;
            for (__int32 i = 0; i < count; ++i)
            {
                __int32 id;
                string name;
                if (!raw.getInt32(id) || id < 0 || !raw.getString(name, 48) || name.empty() || roster.count(id)) return;
                roster[id] = {};
                roster[id].name = SanitiseChatLine(name);
            }
            if (!raw.fullyRead() || static_cast<__int32>(static_cast<unsigned __int32>(revision) - _rosterRevision) < 0) return;
            _rosterRevision = revision;
            _playerIdentities.swap(roster);
            _participantsDirty = true;
            for (auto it = _privateChatKeys.begin(); it != _privateChatKeys.end();)
                if (!_playerIdentities.count(it->first)) { _screens.peerLeft(it->first); _files.peerLeft(it->first); clearFileRelay(it->first); _privateChatNames.erase(it->first); it = _privateChatKeys.erase(it); }
                else ++it;
            return;
        }

        NetworkVoicePacket voicePacket;
        if (ParseVoicePacket(message, messageSize, voicePacket))
        {
            if (saneVoicePacket(voicePacket))
            {
                _voiceActiveUntil = GetTickCount64() + 2000;
                queueVoicePacket(voicePacket);
            }
            return;
        }

        __int32 keyPlayer = -1;
        string keyName;
        string publicKey;
        if (decodeChatKey(message, messageSize, keyPlayer, keyName, publicKey))
        {
            if (keyPlayer >= 0)
            {
                _privateChatKeys[keyPlayer] = publicKey;
                _privateChatNames[keyPlayer] = keyName;
            }
            return;
        }

        __int32 privateFrom = -1;
        string privateName;
        string privateCipher;
        if (decodePrivateChatForClient(message, messageSize, privateFrom, privateName, privateCipher))
        {
            string privateText;
            if (decryptPrivateText(privateCipher, privateText))
            {
                addChatLine(string("(private) ") + privateName + ": " + privateText, CLKPrivate);
                if(privateFrom != localPlayerId() && messageReceived) messageReceived();
            }
            return;
        }

        string text;
        if (ParseAppRawString(message, messageSize, NAMTSessionEnd, text, CHAT_MAX_LINE_CHARS)) {
            addChatLine(text, CLKSystem);
            _remoteEnded = true;
            return;
        }
        if (ParseAppRawString(message, messageSize, NAMTConnect, text, CHAT_MAX_LINE_CHARS) ||
            ParseAppRawString(message, messageSize, NAMTDisconnect, text, CHAT_MAX_LINE_CHARS))
        {
            addChatLine(text, CLKSystem);
            return;
        }
        if (ParseAppRawString(message, messageSize, NAMTSystemNotice, text, CHAT_MAX_LINE_CHARS))
        {
            addChatLine(text, CLKSystem);
            return;
        }
        if (ParseAppRawString(message, messageSize, NAMTCommandError, text, CHAT_MAX_LINE_CHARS))
        {
            addChatLine(text, CLKError);
            return;
        }
        if (ParseAppRawString(message, messageSize, NAMTChat, text, CHAT_MAX_LINE_CHARS))
        {
            addChatLine(SanitiseChatLine(text));
            if(text.find(playerDisplayName(localPlayerId()) + ": ") != 0 && messageReceived) messageReceived();
            return;
        }

        if (ParseAppRawControl(message, messageSize, NAMTHeartbeat))
        {
            return;
        }
        string keyBytes;
        if (ParseAppRawBytes(message, messageSize, NAMTKeyAccept, keyBytes, crypto_kx_PUBLICKEYBYTES))
        {
            if (_clientCrypto.acceptServerKey(keyBytes))
            {
                _client->SetPacketKeys(_clientCrypto.receiveKey(), _clientCrypto.sendKey());
                sendRawIdentityFromClient();
                sendPrivateChatKeyFromClient();
            }
            return;
        }
        addChatLine(string("system: ") + string(message, message + messageSize), CLKSystem);
    }

    void onServerMessage(__int32 from, char* buffer, __int32 bufferSize)
    {
        if(_endingPeers.count(from)||_disconnectDeadline)return;
        // Host authority is local-only, never supplied by a remote identity.
        if (!_server || from <= 0) return;
        string decrypted;
        const char* message = NULL;
        __int32 messageSize = 0;
        if (!prepareServerMessage(from, buffer, bufferSize, decrypted, message, messageSize))
        {
            return;
        }
        if (messageSize < (__int32)sizeof(ChatPacketPrefix))
        {
            return;
        }
        const ChatPacketPrefix* messageHeader = reinterpret_cast<const ChatPacketPrefix*>(message);
        if (messageHeader->type != NAMTKeyHello &&
            messageHeader->type != NAMTConnect &&
            _playerIdentities.find(from) == _playerIdentities.end())
        {
            return;
        }

        if (messageHeader->type == NAMTScreen) { NetPacket raw(message+sizeof(ChatPacketPrefix),messageSize-sizeof(ChatPacketPrefix)); _screens.receive(from,raw); return; }
        if (messageHeader->type == NAMTFile) { handleFileMessage(from,message,messageSize,true); return; }

        // A remote client must not turn one fast input stream into unlimited
        // broadcasts to the whole room. Voice normally sends 50 packets/s.
        auto& budget = _peerBudgets[from];
        if (messageHeader->type == NAMTVoice)
        {
            if (!fileBudget(budget.voice, messageSize, 65536, 60)) return;
        }
        else if (!fileBudget(budget.control, messageSize, 65536, 10)) return;

        NetworkVoicePacket voicePacket;
        if (ParseVoicePacket(message, messageSize, voicePacket))
        {
            if (saneVoicePacket(voicePacket))
            {
                _voiceActiveUntil = GetTickCount64() + 2000;
                voicePacket.playerId = from;
                queueVoicePacket(voicePacket);
                broadcastVoicePacket(voicePacket, from);
            }
            return;
        }

        __int32 keyPlayer = -1;
        string keyName;
        string publicKey;
        if (decodeChatKey(message, messageSize, keyPlayer, keyName, publicKey))
        {
            _privateChatKeys[from] = publicKey;
            _privateChatNames[from] = playerDisplayName(from);
            broadcastChatKey(from, playerDisplayName(from), publicKey);
            return;
        }

        __int32 privateTarget = -1;
        string privateCipher;
        if (decodePrivateChatForServer(message, messageSize, privateTarget, privateCipher))
        {
            routePrivateChat(from, privateTarget, privateCipher);
            return;
        }

        string rawChat;
        if (ParseAppRawString(message, messageSize, NAMTChat, rawChat, CHAT_MAX_MESSAGE_CHARS))
        {
            handleChatFromClient(from, rawChat);
            return;
        }
        string keyBytes;
        if (ParseAppRawBytes(message, messageSize, NAMTKeyHello, keyBytes, crypto_kx_PUBLICKEYBYTES))
        {
            if (_playerIdentities.find(from) != _playerIdentities.end())
            {
                return;
            }
            string response;
            if (_serverCrypto[from].acceptClientHello(keyBytes, response))
            {
                NetPacket raw;
                raw.put(response.data(), (__int32)response.size());
                _server->SetPacketKeys(from, _serverCrypto[from].receiveKey(), _serverCrypto[from].sendKey());
                sendPlainRawFromServer(from, NAMTKeyAccept, raw);
            }
            else
            {
                endPlayerSession(from, DisconnectKicked, "crypto failed");
            }
            return;
        }
        NetworkIdentity rawIdentity;
        if (ParseIdentityRaw(message, messageSize, rawIdentity))
        {
            if (std::find(_bannedSerials.begin(), _bannedSerials.end(), rawIdentity.driveSerial) != _bannedSerials.end())
            {
                endPlayerSession(from, DisconnectBanned, "banned");
                return;
            }
            if (_playerIdentities.find(from) != _playerIdentities.end()) return;
            rawIdentity.name = IdentityDisplayName(rawIdentity.name, from);
            _playerIdentities[from] = rawIdentity;
            sendInitialStateTo(from);
            return;
        }

        if (messageHeader->type == NAMTConnect)
        {
            endPlayerSession(from, DisconnectKicked, "Incompatible identity; update your client.");
            return;
        }

        if (ParseAppRawControl(message, messageSize, NAMTHeartbeat))
        {
            return;
        }
        if (ParseAppRawControl(message, messageSize, NAMTDisconnect))
        {
            _pendingLeaveMessages[from] = playerDisplayName(from) + " disconnected";
            _server->DisconnectPeer(from, DisconnectDisconnected, "Client left.");
            return;
        }

        return;
    }

    void onCreatePlayer(__int32 player, const char* name)
    {
        if (std::find(_players.begin(), _players.end(), player) == _players.end())
        {
            _players.push_back(player);
        }
    }

    void onDeletePlayer(__int32 player)
    {
        _endingPeers.erase(player);
        std::map<__int32, string>::iterator pending = _pendingLeaveMessages.find(player);
        string leaveMessage;
        if (pending != _pendingLeaveMessages.end())
        {
            leaveMessage = pending->second;
            _pendingLeaveMessages.erase(pending);
        }
        else
        {
            leaveMessage = playerDisplayName(player) + " left";
        }

        _players.erase(std::remove(_players.begin(), _players.end(), player), _players.end());
        addChatLine(string("system: ") + leaveMessage, CLKSystem);
        sendRawStringFromServerToAll(NAMTDisconnect, string("system: ") + leaveMessage, CHAT_MAX_LINE_CHARS);

        _playerIdentities.erase(player);
        _screens.peerLeft(player);
        _files.peerLeft(player);
        clearFileRelay(player);
        _peerBudgets.erase(player);
        _privateChatKeys.erase(player);
        _privateChatNames.erase(player);
        _serverCrypto.erase(player);
        broadcastPresence();
    }

    void sendInitialStateTo(__int32 player)
    {
        if (!_server)
        {
            return;
        }
        NetworkPlayerAssignPacket assignPacket;
        assignPacket.playerId = player;
        sendPayloadFromServer(player, BuildAppPacket(NAMTPlayerAssign, assignPacket), DeliveryGuaranteed | DeliveryHighPriority);

        sendKnownChatKeysTo(player);
        _screens.sync(player);
        broadcastPresence();

        std::ostringstream status;
        status << "system: " << playerDisplayName(player) << " joined";
        addChatLine(status.str(), CLKSystem);
        sendRawStringFromServerToAll(NAMTConnect, status.str(), CHAT_MAX_LINE_CHARS);
    }

    string _statusNotice;
    ULONGLONG _statusNoticeUntil = 0;
    void addChatLine(const string& line, ChatLineKind kind = CLKNormal)
    {
        if (line.empty())
        {
            return;
        }
        if (!_hWnd) printf("%s\n", SanitiseChatLine(line).c_str());
        string text = SanitiseChatLine(line);
        if(kind == CLKError || (!isHost() && !clientReady() && kind == CLKSystem)) {
            if(text != "connecting" && text != "disconnected" && text != "Hosting stopped.") {
                _statusNotice = text;
                if(text.find("unknown command:") == 0) _statusNotice = "Unknown command";
                else if(text.find("Not connected.") == 0) _statusNotice = "Disconnected";
                else if(text.find("usage:") == 0) {
                    const size_t end = text.find(' ', 7);
                    _statusNotice = "Check " + text.substr(7, end == string::npos ? end : end-7) + " syntax";
                }
                else if(text.find("Failed to join.") == 0) _statusNotice = "Connection failed";
                else if(text.find("Cannot read the C:") == 0) _statusNotice = "Cannot read drive ID";
                else if(text.find("Host name must") == 0) _statusNotice = "Invalid host name";
                else if(text.find("Dedicated on:") == 0) _statusNotice = "Auto-host enabled";
                else if(text.find("Dedicated off:") == 0) _statusNotice = "Auto-host disabled";
                else if(text.find("No recipients are ready") == 0) _statusNotice = "No file recipients";
                _statusNoticeUntil = GetTickCount64() + 6000;
            }
        }
        size_t offset = 0;
        while (offset < text.size())
        {
            size_t remaining = text.size() - offset;
            size_t count = CHAT_WRAP_CHARS < remaining ? CHAT_WRAP_CHARS : remaining;
            if (offset + count < text.size())
            {
                size_t split = text.rfind(' ', offset + count);
                if (split != string::npos && split > offset)
                {
                    count = split - offset;
                }
            }
            NetworkChatLine chatLine;
            chatLine.text = text.substr(offset, count);
            chatLine.kind = kind;
            chatLine.historyId = ++_nextChatLineId; GetLocalTime(&chatLine.receivedAt);
            _chatLines.push_back(chatLine);
            offset += count;
            while (offset < text.size() && text[offset] == ' ')
            {
                ++offset;
            }
        }
        while (_chatLines.size() > CHAT_MAX_HISTORY_LINES)
        {
            _chatLines.erase(_chatLines.begin());
        }
    }

    bool sendRawFromClient(ChatPacketKind type, const NetPacket& raw, DeliveryOptions flags)
    {
        if (!_client)
        {
            return false;
        }
        string payload = BuildAppRawMessage(type, raw);
        string encrypted;
        if (!_clientCrypto.encrypt(payload, encrypted))
        {
            return false;
        }
        DWORD msgID = 0;
        return (bool)_client->SendPayload((BYTE*)encrypted.data(), (__int32)encrypted.size(), msgID, flags, IntrusivePtr<PacketBuffer>());
    }

    bool sendRawFromServer(__int32 player, ChatPacketKind type, const NetPacket& raw, DeliveryOptions flags)
    {
        return sendPayloadFromServer(player, BuildAppRawMessage(type, raw), flags);
    }

    bool sendPayloadFromServer(__int32 player, const string& payload, DeliveryOptions flags)
    {
        if (!_server)
        {
            return false;
        }
        std::map<__int32, cCryptoSession>::iterator found = _serverCrypto.find(player);
        string encrypted;
        if (found == _serverCrypto.end() || !found->second.encrypt(payload, encrypted))
        {
            return false;
        }
        DWORD msgID = 0;
        return (bool)_server->SendPayload(player, (BYTE*)encrypted.data(), (__int32)encrypted.size(), msgID, flags, IntrusivePtr<PacketBuffer>());
    }

    void sendRawStringFromClient(ChatPacketKind type, const string& message, size_t maxLength)
    {
        NetPacket raw;
        raw.putString(message, maxLength);
        sendRawFromClient(type, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void sendRawStringFromServer(__int32 player, ChatPacketKind type, const string& message, size_t maxLength)
    {
        NetPacket raw;
        raw.putString(message, maxLength);
        sendRawFromServer(player, type, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void sendRawStringFromServerToAll(ChatPacketKind type, const string& message, size_t maxLength)
    {
        for (size_t i = 0; i < _players.size(); ++i)
        {
            sendRawStringFromServer(_players[i], type, message, maxLength);
        }
    }

    void sendRawControlFromClient(ChatPacketKind type)
    {
        NetPacket raw;
        sendRawFromClient(type, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void sendRawControlFromServer(__int32 player, ChatPacketKind type)
    {
        NetPacket raw;
        sendRawFromServer(player, type, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void sendRawIdentityFromClient()
    {
        NetPacket raw;
        EncodeLocalIdentityRaw(raw);
        sendRawFromClient(NAMTConnect, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void sendPrivateChatKeyFromClient()
    {
        if (!_client || !ensurePrivateChatKey())
        {
            return;
        }
        NetPacket raw;
        encodeChatKey(raw, 0, LocalUserName(), privateChatPublicKeyBytes());
        sendRawFromClient(NAMTChatKey, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
    }

    void sendVoicePayloadFromClient(const NetworkVoicePacket& packet)
    {
        if (!_client)
        {
            return;
        }
        string payload = BuildVoicePayload(packet);
        string encrypted;
        if (!_clientCrypto.encrypt(payload, encrypted))
        {
            return;
        }
        payload = encrypted;
        DWORD msgID = 0;
        _client->SendPayload((BYTE*)payload.data(), (__int32)payload.size(), msgID, DeliveryNone, IntrusivePtr<PacketBuffer>());
    }

    void sendVoicePayloadFromServer(__int32 player, const NetworkVoicePacket& packet)
    {
        if (!_server)
        {
            return;
        }
        string payload = BuildVoicePayload(packet);
        std::map<__int32, cCryptoSession>::iterator found = _serverCrypto.find(player);
        string encrypted;
        if (found == _serverCrypto.end() || !found->second.encrypt(payload, encrypted))
        {
            return;
        }
        payload = encrypted;
        DWORD msgID = 0;
        _server->SendPayload(player, (BYTE*)payload.data(), (__int32)payload.size(), msgID, DeliveryNone, IntrusivePtr<PacketBuffer>());
    }

    void broadcastVoicePacket(const NetworkVoicePacket& packet, __int32 exceptPlayer)
    {
        for (size_t i = 0; i < _players.size(); ++i)
        {
            if (_players[i] != exceptPlayer)
            {
                sendVoicePayloadFromServer(_players[i], packet);
            }
        }
    }

    string playerDisplayName(__int32 player) const
    {
        if (player == 0 && _server)
        {
            return _hostName;
        }
        std::map<__int32, NetworkIdentity>::const_iterator found = _playerIdentities.find(player);
        if (found != _playerIdentities.end() && !found->second.name.empty())
        {
            return found->second.name;
        }
        std::ostringstream fallback;
        fallback << "player " << player;
        return fallback.str();
    }

    bool resolvePlayerReference(const string& reference, __int32& player, bool allowBaseName = false) const
    {
        string value = TrimWhitespace(reference);
        if (value.empty())
        {
            return false;
        }

        char* end = NULL;
        long numeric = strtol(value.c_str(), &end, 10);
        if (end && *end == 0 &&
            (std::find(_players.begin(), _players.end(), (__int32)numeric) != _players.end() ||
             _privateChatKeys.find((__int32)numeric) != _privateChatKeys.end()))
        {
            player = (__int32)numeric;
            return true;
        }

        for (std::map<__int32, NetworkIdentity>::const_iterator i = _playerIdentities.begin(); i != _playerIdentities.end(); ++i)
        {
            if (_stricmp(i->second.name.c_str(), value.c_str()) == 0)
            {
                player = i->first;
                return true;
            }
        }
        if (allowBaseName)
        {
            __int32 match = -1;
            bool found = false;
            for (std::map<__int32, NetworkIdentity>::const_iterator i = _playerIdentities.begin(); i != _playerIdentities.end(); ++i)
            {
                const string& name = i->second.name;
                size_t prefix = name.find(':');
                if (prefix == string::npos || _stricmp(name.substr(prefix + 1).c_str(), value.c_str()) != 0) continue;
                if (found) return false;
                match = i->first;
                found = true;
            }
            if (found) { player = match; return true; }
        }
        return false;
    }

    void sendCommandResult(__int32 player, const string& message, bool error = false)
    {
        if (player == 0 && std::find(_players.begin(), _players.end(), player) == _players.end())
        {
            addChatLine(string("system: ") + message, error ? CLKError : CLKSystem);
            return;
        }
        sendRawStringFromServer(player, error ? NAMTCommandError : NAMTSystemNotice, string("system: ") + message, CHAT_MAX_LINE_CHARS);
    }

    void handleChatFromClient(__int32 from, string text)
    {
        text = SanitiseChatText(text);
        if (text.empty())
        {
            return;
        }
        if (text[0] == '/')
        {
            runPlayerCommand(from, text);
            return;
        }
        std::ostringstream line;
        line << playerDisplayName(from) << ": " << text;
        addChatLine(line.str());
        if(from != localPlayerId() && messageReceived) messageReceived();
        sendRawStringFromServerToAll(NAMTChat, line.str(), CHAT_MAX_LINE_CHARS);
    }

    void routePrivateChat(__int32 from, __int32 target, const string& cipher)
    {
        if (!_server || cipher.empty())
        {
            return;
        }
        const string fromName = playerDisplayName(from);
        if (target == 0)
        {
            string privateText;
            if (decryptPrivateText(cipher, privateText))
            {
                addChatLine(fromName + ": " + privateText, CLKPrivate);
                if(from != localPlayerId() && messageReceived) messageReceived();
            }
            return;
        }
        if (std::find(_players.begin(), _players.end(), target) == _players.end())
        {
            sendCommandResult(from, "private target not found", true);
            return;
        }
        sendPrivateChatToClient(target, from, fromName, cipher);
    }

    bool sendPrivateChatByReferenceInternal(const string& reference, const string& message)
    {
        __int32 target = -1;
        if (!resolvePlayerReference(reference, target, true))
        {
            addChatLine("system: private target not found", CLKError);
            return false;
        }
        string text = SanitiseChatText(message);
        string cipher;
        if (text.empty() || !encryptPrivateText(target, text, cipher))
        {
            addChatLine("system: private key unavailable", CLKError);
            return false;
        }
        if (_client)
        {
            NetPacket raw;
            raw.putInt32(target);
            raw.putString(cipher, 255);
            sendRawFromClient(NAMTPrivateChat, raw, (DeliveryOptions)(DeliveryGuaranteed | DeliveryHighPriority));
            addChatLine(string(">") + playerDisplayName(target) + ": " + text, CLKPrivate);
            return true;
        }
        if (_server)
        {
            sendPrivateChatToClient(target, 0, _hostName, cipher);
            addChatLine(string(">") + playerDisplayName(target) + ": " + text, CLKPrivate);
            return true;
        }
        return false;
    }

    void sendUsersList(__int32 to)
    {
        std::ostringstream summary;
        const auto people = participants();
        summary << "users online: " << people.size();
        sendCommandResult(to, summary.str());
        for (const auto& person : people)
        {
            __int32 player = person.first;
            __int32 latency = 0;
            __int32 throughput = 0;
            if (player == 0 || !_server || !_server->QueryConnectionMetrics(player, latency, throughput))
            {
                latency = 0;
            }
            std::ostringstream line;
            line << person.second;
            if (player == 0) line << " (host)";
            else line << " " << latency << "ms";
            sendCommandResult(to, line.str());
        }
    }

    void runPlayerCommand(__int32 from, const string& command)
    {
        if (_stricmp(command.c_str(), "/help") == 0)
        {
            if (from == 0)
            {
                for (const auto& entry : CHAT_COMMANDS)
                    if (entry.hostOnly) sendCommandResult(from, entry.help);
            }
            return;
        }
        if (MatchesCommand(command, "/name"))
        {
            std::map<__int32, NetworkIdentity>::iterator identity = _playerIdentities.find(from);
            if (from != 0 && identity == _playerIdentities.end())
            {
                sendCommandResult(from, "Player not found.", true);
                return;
            }
            string name = TrimWhitespace(command.substr(5));
            if (name.empty())
            {
                sendCommandResult(from, "usage: /name username", true);
                return;
            }
            if (!ValidUserName(name))
            {
                sendCommandResult(from, "usage: /name username (1-32 letters: a-z, A-Z only)", true);
                return;
            }
            string& displayName = from == 0 ? _hostName : identity->second.name;
            string oldName = displayName;
            displayName = IdentityDisplayName(name, from);
            broadcastPresence();
            _privateChatNames[from] = displayName;
            std::map<__int32, string>::const_iterator key = _privateChatKeys.find(from);
            if (key != _privateChatKeys.end()) broadcastChatKey(from, displayName, key->second);
            string notice = "system: " + oldName + " is now " + displayName;
            addChatLine(notice, CLKSystem);
            sendRawStringFromServerToAll(NAMTSystemNotice, notice, CHAT_MAX_LINE_CHARS);
            return;
        }
        if (_stricmp(command.c_str(), "/users") == 0)
        {
            sendUsersList(from);
            return;
        }
        if (from != 0)
        {
            sendCommandResult(from, "host only command", true);
            return;
        }

        if (MatchesCommand(command, "/kick"))
        {
            string argument = TrimWhitespace(command.size() > 5 ? command.substr(5) : string());
            if (argument.empty())
            {
                sendCommandResult(from, "usage: /kick name|netId:name|netId", true);
                return;
            }
            const bool sent = kickPlayer(argument, false);
            sendCommandResult(from, sent ? "kick sent" : "Player not found or name ambiguous; use netId:name or netId.", !sent);
            return;
        }
        if (MatchesCommand(command, "/ban"))
        {
            string argument = TrimWhitespace(command.size() > 4 ? command.substr(4) : string());
            if (argument.empty())
            {
                sendCommandResult(from, "usage: /ban name|netId:name|netId", true);
                return;
            }
            const bool sent = kickPlayer(argument, true);
            sendCommandResult(from, sent ? "ban sent" : "Player not found or name ambiguous; use netId:name or netId.", !sent);
            return;
        }

        sendCommandResult(from, string("unknown command: ") + command, true);
    }

public:
    cNetworkRuntime(HWND hWnd, PackedClientSettings* settings = NULL)
        : _screens([this] { return isHost(); }, [this] { return localPlayerId(); },
            [this] { return _players; },
            [this](int to,const NetPacket& raw) {
                NetPacket header(raw.data(),raw.size());int kind=0;if(!header.getInt32(kind))return;
                if(kind==ScreenSignaling::Media) {
                    int messages=0,bytes=0,guaranteedMessages=0,guaranteedBytes=0;
                    if(isHost()) _server->QueryPendingSends(to,messages,bytes,guaranteedMessages,guaranteedBytes);
                    else if(_client) _client->QueryPendingSends(messages,bytes,guaranteedMessages,guaranteedBytes);
                    // Bound the realtime queue independently. Reliable file backlog must
                    // not suppress screen media or WebRTC's loss/connection recovery traffic.
                    if(messages>=64 || bytes>=65536)return;
                }
                auto flags=kind==ScreenSignaling::Media?DeliveryNone:DeliveryGuaranteed;
                if(isHost()) { if(_playerIdentities.count(to)) sendRawFromServer(to,NAMTScreen,raw,flags); }
                else if(clientReady()) sendRawFromClient(NAMTScreen,raw,flags);
            },
            [this](int owner,const string& id) {
                NetworkChatLine line; line.kind=CLKScreen; line.fileSender=owner; line.fileId=id;
                line.historyId = ++_nextChatLineId; GetLocalTime(&line.receivedAt);
                _chatLines.push_back(line);
                if(_chatLines.size()>CHAT_MAX_HISTORY_LINES) _chatLines.erase(_chatLines.begin());
            }),
          _files([this](int to, const vector<unsigned char>& bytes) { return sendFilePayload(to,bytes); },
                 [this](const string& text, bool error) { showNotice(text,error); },
                 [this](int from, const string& id) { announceFile(from,id); }),
          _hWnd(hWnd),
          _settings(settings),
          _server(NULL),
          _client(NULL),
          _nextClientHeartbeat(0),
          _nextServerBroadcast(0),
          _localPlayerId(-1),
          _latencyMS(0),
          _throughputBPS(0),
          _privateChatKeyReady(false)
    {
        sodium_init();
        _files.viewImage=[this](std::shared_ptr<const Screenshot> image) {
            if(!_imageViewer.show(std::move(image)))showNotice("Could not open image.",true);
        };
        ensurePrivateChatKey();

    }

    ~cNetworkRuntime()
    {
        if (_client || _server) { disconnect(); finishDisconnect(); }
    }

    bool hostOnPort(unsigned short port = DEFAULT_NETWORK_PORT)
    {
        _statusNotice.clear();
        _statusNoticeUntil = 0;
        if (_server || _client)
        {
            addChatLine("network already active", CLKError);
            return false;
        }

        _hostName = IdentityDisplayName(LocalUserName(), 0);
        _players.clear();
        _playerIdentities.clear();
        _participantsDirty = true;
        _rosterRevision = 0;
        _privateChatKeys.clear();
        _privateChatNames.clear();
        _pendingLeaveMessages.clear();
        _serverCrypto.clear();
        _clientCrypto.clear();
        _voicePackets.clear();
        _privateChatKeyReady = false;
        ensurePrivateChatKey();
        _nextServerBroadcast = 0;
        _nextHostStatistics = 0;
        _server = MakeHostSession();
        if (!_server || !_server->StartSession("oi", "", port))
        {
            addChatLine("host failed", CLKError);
            Error("Network host failed on port %u", port);
            stopUdpWorkers();
            delete _server;
            _server = NULL;
            releaseTransportRegistry();
            return false;
        }

        _hostingPort = port;
        ReadBlockedSerials(_bannedSerials);
        return true;
    }

    bool connectTo(const string& requestedAddress)
    {
        _statusNotice.clear();
        _statusNoticeUntil = 0;
        if (_client || _server)
        {
            addChatLine("network already active", CLKError);
            return false;
        }

        if (LocalDriveSerial().empty())
        {
            addChatLine("Cannot read the C: volume identity.", CLKError);
            return false;
        }
        const string address = requestedAddress.empty()
            ? (_settings ? _settings->serverAddress : DEFAULT_NETWORK_ADDRESS) : requestedAddress;
        unsigned short port = DEFAULT_NETWORK_PORT;
        string host;
        if (!parseServerAddress(address, host, port))
        {
            showNotice("Invalid server address or port.", true);
            return false;
        }
        _client = MakeClientSession();
        JoinResult result = _client ? _client->StartSession(address, "", false, port, "oi", NULL) : JoinError;
        if (result != JoinOK && result != JoinNone)
        {
            addChatLine(string("Failed to join. Error: ") + ConnectResultName(result), CLKError);
            stopUdpWorkers();
            delete _client;
            _client = NULL;
            releaseTransportRegistry();
            return false;
        }

        _connectingAddress = address;
        _transportConnecting = result == JoinNone;
        _connectDeadline = GetTickCount64() + 10000;
        addChatLine("connecting", CLKSystem);
        if (!_transportConnecting) beginClientCryptoHandshake();
        return true;
    }

    void update()
    {
        if(_disconnectDeadline){
            if(GetTickCount64()>=_disconnectDeadline||!outgoingPending())finishDisconnect();
            return;
        }
        drainEndingPeers();
        flushFileRelay();
        int filePeer; string fileId; std::wstring destination;
        if (_fileDialog.poll(filePeer,fileId,destination) && !destination.empty()) _files.accept({filePeer,fileId},destination);
        if (_client && _connectDeadline && GetTickCount64() >= _connectDeadline)
        {
            addChatLine("connection timed out", CLKError);
            _remoteEnded = true;
            disconnect();
            return;
        }
        if (_client && _transportConnecting)
        {
            JoinResult result = _client->PollJoin();
            if (result == JoinNone) return;
            _transportConnecting = false;
            if (result != JoinOK)
            {
                addChatLine(result == JoinTimeout ? "connection timed out" :
                    string("Failed to join. Error: ") + ConnectResultName(result), CLKError);
                _remoteEnded = true;
                disconnect();
                return;
            }
            beginClientCryptoHandshake();
        }
        if (_server)
        {
            _server->DrainPeerEvents(OnPeerJoined, OnPeerLeft, this);
            _server->DrainIncomingPayloads(OnServerMessage, this);

            unsigned __int64 now = GetTickCount64();
            if (!_players.empty() && now >= _nextServerBroadcast)
            {
                for (size_t i = 0; i < _players.size(); ++i)
                {
                    sendRawControlFromServer(_players[i], NAMTHeartbeat);
                }
                _nextServerBroadcast = now + 5000;
            }
        }

        if (_client)
        {
            _client->DrainIncomingPayloads(OnClientMessage, this);
            _client->QueryConnectionMetrics(_latencyMS, _throughputBPS);
            if (_remoteEnded || _client->HasDisconnected())
            {
                string reason = _client->GetWhySessionTerminatedStr();
                if (!_remoteEnded && !reason.empty()) addChatLine(reason, CLKError);
                _remoteEnded = true;
                disconnect();
                return;
            }
            else
            {
                unsigned __int64 now = GetTickCount64();
                if (now >= _nextClientHeartbeat)
                {
                    sendRawControlFromClient(NAMTHeartbeat);
                    _nextClientHeartbeat = now + 3000;
                }
            }
        }

        const auto statisticsNow = GetTickCount64();
        if (_server && statisticsNow >= _nextHostStatistics)
        {
            _nextHostStatistics = statisticsNow + 1000;
            __int64 totalLatency = 0;
            __int64 totalThroughput = 0;
            __int32 counted = 0;
            for (size_t i = 0; i < _players.size(); ++i)
            {
                __int32 latency = 0;
                __int32 throughput = 0;
                if (_server->QueryConnectionMetrics(_players[i], latency, throughput))
                {
                    totalLatency += latency;
                    totalThroughput += throughput;
                    ++counted;
                }
            }
            _latencyMS = counted ? (__int32)(totalLatency / counted) : 0;
            _throughputBPS = counted ? (__int32)(totalThroughput / counted) : 0;
        }
        _files.update(GetTickCount64() < _voiceActiveUntil);
    }

    bool trafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const
    {
        incoming = outgoing = 0;
        if (_server) _server->GetTrafficTotals(incoming, outgoing);
        else if (_client) _client->GetTrafficTotals(incoming, outgoing);
        else return false;
        return true;
    }

    const std::vector<std::pair<__int32, string>>& participants() const
    {
        if (!_participantsDirty) return _participantCache;
        _participantCache.clear();
        if (_server) _participantCache.push_back({0, _hostName});
        if (_server || clientReady())
            for (const auto& entry : _playerIdentities) _participantCache.push_back({entry.first, entry.second.name});
        _participantsDirty = false;
        return _participantCache;
    }

    __int32 localPlayerId() const { return _server ? 0 : _localPlayerId; }

    string statusNotice() const {return GetTickCount64() < _statusNoticeUntil ? _statusNotice : string();}
    std::function<void()> messageReceived;
    void showNotice(const string& text, bool error = false) { addChatLine(text, error ? CLKError : CLKSystem); }

    bool clientReady() const { return !_disconnectDeadline && _client && _localPlayerId >= 0; }

    __int32 latencyMS() const
    {
        return _latencyMS;
    }

    __int32 throughputBPS() const
    {
        return _throughputBPS;
    }

    const vector<NetworkChatLine>& chatLines() const
    {
        return _chatLines;
    }

    bool hasConnection() const
    {
        return _server != NULL || _client != NULL;
    }

    bool isConnectedClient() const
    {
        return _client != NULL;
    }

    void setDedicated(const string& argument)
    {
        const string mode = TrimWhitespace(argument);
        if (mode != "on" && mode != "off" && !mode.empty()) { showNotice("usage: /dedicated [on|off]", true); return; }
        if (!_settings) return;
        const bool enable = mode.empty() ? !_settings->dedicated() : mode == "on";
        if (enable && !isHost())
        {
            if (hasConnection()) { showNotice("Use /disconnect before enabling dedicated mode.", true); return; }
            if (!hostOnPort()) return;
        }
        if (!_settings->setDedicated(enable)) { showNotice("Could not save startup setting.", true); return; }
        showNotice(enable ? "Dedicated on: host automatically at startup." : "Dedicated off: automatic hosting disabled.");
    }

    void setHostName(const string& argument) {
        const auto name=TrimWhitespace(argument);
        if(!_settings)return;
        if(name.empty()){showNotice("Host name: "+_settings->hostName);return;}
        if(!HostDirectory::validName(name.c_str())){showNotice("Host name must be 1-32 printable characters.",true);return;}
        if (!_settings->setHostName(name)) { showNotice("Could not save host name.", true); return; }
        showNotice("Host name: " + name);
    }
    void changeName(const string& argument)
    {
        string name = TrimWhitespace(argument);
        if (name.empty())
        {
            addChatLine("usage: /name username", CLKError);
            return;
        }
        if (!ValidUserName(name))
        {
            addChatLine("usage: /name username (1-32 letters: a-z, A-Z only)", CLKError);
            return;
        }
        if (name != LocalUserName() && !SaveClientFile("ClientName.txt", name + "\n"))
        {
            addChatLine("Could not save your name.", CLKError);
            return;
        }
        if (_client || _server) sendChat("/name " + name);
        else addChatLine("Name saved: " + name, CLKSystem);
    }

    void offerFile(const std::wstring& path) {
        vector<int> peers;
        if (isHost() || clientReady()) for (const auto& person : participants())
            if (person.first != localPlayerId() && _privateChatKeys.count(person.first)) peers.push_back(person.first);
        _files.offer(path,peers);
    }
    void offerScreenshot() {
        vector<int> peers;
        if(isHost() || clientReady())for(const auto& person:participants())
            if(person.first!=localPlayerId() && _privateChatKeys.count(person.first))peers.push_back(person.first);
        if(peers.empty()){showNotice("No recipients are ready for images.",true);return;}
        auto image=Screenshot::clipboard();
        if(!image){showNotice("Cannot paste image: unsupported format, too large, or screenshot memory is full.",true);return;}
        _files.offerImage(std::move(image),peers);
    }
    unsigned short hostingPort() const { return _hostingPort; }
    std::function<void()> browseHosts;
    std::function<void(int,const string&)> openScreen;
    void sendScreenEvent(const ScreenSignaling::Event& event) { if(isHost() || clientReady()) _screens.send(event); }
    bool popScreenEvent(ScreenSignaling::Event& event) { return _screens.pop(event); }
    string screenLabel(int owner,const string& id) const {
        return playerDisplayName(owner) + " screen [" + (_screens.available(owner,id) ? "watch" : "ended") + "]";
    }
    void clickScreen(int owner,const string& id) {
        if(_screens.available(owner,id) && openScreen) openScreen(owner,id);
        else showNotice("This screen share has ended.",true);
    }
    string fileLabel(int from, const string& id) const { return _files.label({from,id}); }
    void clickFile(int from, const string& id) {
        if (from == -1) { _files.withdraw(id); return; }
        const FileTransfers::Key key(from,id);
        if (_files.active(key)) { _files.cancel(key); return; }
        if(_files.isImage(key)){if(!_files.view(key))showNotice("This image is no longer available.",true);return;}
        const auto name = _files.suggestedName(key);
        if (name.empty()) { showNotice("This file offer is no longer available.",true); return; }
        if (!_fileDialog.start(from,id,name)) showNotice("A save dialog is already open.",true);
    }

    bool sendPrivateChatByReference(const string& reference, const string& message)
    {
        return sendPrivateChatByReferenceInternal(reference, message);
    }

    void sendChat(const string& text)
    {
        string message = SanitiseChatText(text);
        if (message.empty())
        {
            return;
        }

        if (_client)
        {
            if (!clientReady()) { addChatLine("Still connecting.", CLKError); return; }
            sendRawStringFromClient(NAMTChat, message, CHAT_MAX_MESSAGE_CHARS);
        }
        else if (_server)
        {
            if (message[0] == '/')
            {
                runPlayerCommand(0, message);
                return;
            }
            string line = _hostName + ": " + message;
            addChatLine(line, CLKNormal);
            sendRawStringFromServerToAll(NAMTChat, line, CHAT_MAX_LINE_CHARS);
        }
        else addChatLine("Not connected. Use /connect or /host.", CLKError);
    }

    bool kickPlayer(const string& reference, bool ban)
    {
        if (!_server)
        {
            addChatLine("host only command", CLKError);
            return false;
        }
        __int32 player = -1;
        if (!resolvePlayerReference(reference, player, true))
        {
            addChatLine("Player not found or name ambiguous; use netId:name or netId.", CLKError);
            return false;
        }
        const string displayName = playerDisplayName(player);
        if (ban)
        {
            std::map<__int32, NetworkIdentity>::const_iterator identity = _playerIdentities.find(player);
            if (identity != _playerIdentities.end() &&
                AddUniqueString(_bannedSerials, identity->second.driveSerial))
            {
                WriteBlockedSerials(_bannedSerials);
            }
        }
        std::ostringstream line;
        line << displayName << (ban ? " was banned" : " was kicked");
        _pendingLeaveMessages[player] = line.str();
        endPlayerSession(player, ban ? DisconnectBanned : DisconnectKicked, line.str().c_str());
        return true;
    }



    bool isHost() const
    {
        return !_disconnectDeadline && _server != NULL;
    }

    void sendVoice(NetworkVoicePacket packet)
    {
        if (clientReady() || isHost()) _voiceActiveUntil = GetTickCount64() + 2000;
        if (_client)
        {
            packet.playerId = _localPlayerId;
            sendVoicePayloadFromClient(packet);
        }
        else if (_server)
        {
            packet.playerId = 0;
            broadcastVoicePacket(packet, -1);
        }
    }

    bool consumeVoicePacket(NetworkVoicePacket& packet)
    {
        if (_voicePackets.empty())
        {
            return false;
        }
        auto first = _voicePackets.begin();
        packet = first->second.front();
        first->second.pop_front();
        if (first->second.empty()) _voicePackets.erase(first);
        return true;
    }

    void clearChat()
    {
        _chatLines.clear();
    }

    bool disconnect()
    {
        if (!_client && !_server) return false;
        if(_disconnectDeadline)return true;
        _screens.clear();
        _imageViewer.clear(); _files.clear(); _fileRelay.clear(); _fileRelayBytes = 0; _peerBudgets.clear(); _voiceActiveUntil = 0;
        _transportConnecting = false;
        _connectDeadline = 0;
        if (_client && !_remoteEnded) sendRawControlFromClient(NAMTDisconnect);
        if (_server)
            sendRawStringFromServerToAll(NAMTSessionEnd, "system: Host stopped the server.", CHAT_MAX_LINE_CHARS);
        _disconnectDeadline=GetTickCount64()+250;
        return true;
    }
private:
    void finishDisconnect()
    {
        const bool hosting=_server!=nullptr;
        // Receive callbacks use the transport objects. Join their workers
        // before clearing pointers or freeing either object.
        stopUdpWorkers();
        delete _client;
        delete _server;
        _client = NULL;
        _server = NULL;
        releaseTransportRegistry();
        _disconnectDeadline=0;_endingPeers.clear();
        _remoteEnded = false;
        _localPlayerId = -1;
        _players.clear();
        _playerIdentities.clear();
        _participantsDirty = true;
        _rosterRevision = 0;
        _privateChatKeys.clear();
        _privateChatNames.clear();
        _pendingLeaveMessages.clear();
        _serverCrypto.clear();
        _clientCrypto.clear();
        _voicePackets.clear();
        _privateChatKeyReady = false;
        ensurePrivateChatKey();
        _nextClientHeartbeat = _nextServerBroadcast = 0;
        _nextHostStatistics = 0;
        _latencyMS = _throughputBPS = 0;
        addChatLine(hosting ? "Hosting stopped." : "disconnected", CLKSystem);
    }
};
