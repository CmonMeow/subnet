
#include "../sysdef.h"
#include "SessionTransport.hpp"
#include <time.h>
#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <vector>
#include "TransportIncludes.hpp"
#include "UdpEndpoint.hpp"
#include "ReliableChannel.hpp"
#include "InboundQueue.hpp"
#include "AsyncResolver.h"


class ClientSession;
class HostSession;

unsigned short DefaultListenPort() { return 777; }
IntrusivePtr<PacketBuffer> assembleFragments(PacketBuffer* msg);
EndpointRegistry* endpointRegistry();

static __int32 GenerateServerChallenge(const sockaddr_in& distant)
{
	static volatile LONG counter = 0;
	typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
	static RtlGenRandomFn rtlGenRandom = NULL;
	static bool randomProviderLoaded = false;
	if (!randomProviderLoaded)
	{
		HMODULE advapi = LoadLibraryA("advapi32.dll");
		if (advapi)
			rtlGenRandom = reinterpret_cast<RtlGenRandomFn>(GetProcAddress(advapi, "SystemFunction036"));
		randomProviderLoaded = true;
	}
	unsigned __int32 randomValue = 0;
	if (rtlGenRandom && rtlGenRandom(&randomValue, sizeof(randomValue)))
	{
		randomValue &= 0x7fffffff;
		return randomValue == 0 ? 1 : static_cast<__int32>(randomValue);
	}

	LARGE_INTEGER qpc;
	QueryPerformanceCounter(&qpc);

	unsigned __int64 value = static_cast<unsigned __int64>(qpc.QuadPart);
	value ^= GetTickCount64() + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
	value ^= static_cast<unsigned __int64>(InterlockedIncrement(&counter)) * 0xbf58476d1ce4e5b9ull;
	value ^= static_cast<unsigned __int64>(distant.sin_addr.s_addr) << 17;
	value ^= static_cast<unsigned __int64>(distant.sin_port) << 41;
	value ^= static_cast<unsigned __int64>(GetCurrentProcessId()) << 9;
	value ^= static_cast<unsigned __int64>(GetCurrentThreadId()) << 33;

	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ull;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebull;
	value ^= value >> 31;

	__int32 challenge = static_cast<__int32>(value & 0x7fffffff);
	return challenge == 0 ? 1 : challenge;
}

#ifndef BROADCAST_PEER_ID
#define BROADCAST_PEER_ID 0
#endif

#define SESSION_NAME_LEN 256
#define GAME_TYPE_NAME_LEN 8
#define MISSION_NAME_LEN 40

#define RESERVED_PEER_ID_COUNT 1

#define PLAYER_NAME_LEN 40
#define PASSWORD_LEN 40



#define JOIN_REPLY_TIMEOUT_MS 8000

#define JOIN_RETRY_INTERVAL_MS 2000


#define SESSION_TEARDOWN_WAIT_MS 500

#define PEER_REMOVAL_WAIT_MS 100

// Unreliable voice/video must not accumulate seconds of stale playback.
constexpr unsigned BestEffortLifetimeMs = 200;

#define PACKET_HANDSHAKE 0x0001



#define JOIN_PROBE_TAG 0x4F490101u

#define JOIN_CHALLENGE_TAG 0x4F490102u

#define JOIN_RESPONSE_TAG 0x4F490103u

#define JOIN_ACCEPT_TAG 0x4F490104u




#pragma pack(push, netPackets, 1)

struct HandshakePrefix 
{
	unsigned __int32 magic;
};

struct RoomAdvertisement : public HandshakePrefix 
{
	char name[SESSION_NAME_LEN];
	__int32 serverState;
	__int32 roomCapacity;
	short password;
	short port;
	__int32 connectedPeerCount;
	unsigned __int32 request;
};

struct JoinChallenge : public HandshakePrefix 
{
	__int32 challenge;
};

struct JoinRequest : public HandshakePrefix 
{
	char name[PLAYER_NAME_LEN];
	char password[PASSWORD_LEN];
	__int32 actualBuild;
	short reservedBotRole; 
};

struct VerifiedJoinRequest : public JoinRequest 
{
	__int32 challenge;
};

struct JoinReply : public HandshakePrefix 
{
	__int32 result;
	__int32 assignedPeerId;
};



#pragma pack(pop, netPackets)

static EndpointInterface* clientEndpoint();

std::recursive_mutex& poolCriticalSection()
{
	static std::recursive_mutex stateMutex;
	return stateMutex;
}

class ClientSession : public ClientSessionInterface
{
    AsyncResolver resolver;
    sockaddr_in connectingAddress = {};
    VerifiedJoinRequest connectPacket = {};
    unsigned __int64 connectDeadline = 0, nextConnectSend = 0;
    bool resolving = false, challengeSent = false;
    JoinResult connectResult = JoinError;
    JoinCancellationCheck* cancelConnect = nullptr;


protected:
	
	unsigned __int64 lastReceiveReportedMs;

	IntrusivePtr<ChannelInterface> channel;

	__int32 challengePlayer;
	
	__int32 joinReplyCode;

	__int32 assignedPeerId;

	bool localBotRole;

	bool sessionClosed;

	DisconnectReason sessionCloseReason;

	char whySessionTerminatedStr[512] = {};

	IncomingPacketQueue alreadyReceived;

	void enqueueIncomingPayload(PacketBuffer* msg);

	IntrusivePtr<PacketBuffer> split;

	PacketBuffer* receiveFragmentsTail;

	IntrusivePtr<PacketBuffer> priorityReceiveFragments;

	PacketBuffer* priorityReceiveFragmentsTail;

	mutable std::recursive_mutex inboundMutex;

	IntrusivePtr<PacketBuffer> sent;

	mutable std::recursive_mutex receiptMutex;

	inline void enterAll() const
	{
		poolCriticalSection().lock();
		EndpointInterface* peer = clientEndpoint();
		if (peer)
			peer->lockEndpoint();
		receiptMutex.lock();
	}
	inline void leaveAll() const
	{
		receiptMutex.unlock();
		EndpointInterface* peer = clientEndpoint();
		if (peer)
			peer->unlockEndpoint();
		poolCriticalSection().unlock();
	}

	friend PacketStatus onClientPacket(PacketBuffer* msg, PacketStatus event, void* data);
	friend PacketStatus challengeReceive(PacketBuffer* msg, PacketStatus event, void* data);
	friend PacketStatus onClientSendFinished(PacketBuffer* msg, PacketStatus event, void* data);

	bool SendMagicPacket(const void* data, size_t size);

public:
	
	ClientSession();

	virtual ~ClientSession();

	virtual JoinResult StartSession(
		std::string address, std::string password, bool reservedBotRole, unsigned short& port,
		std::string player, JoinCancellationCheck* cancelNNCallback = NULL);


	JoinResult PollJoin() override;

    void SetPacketKeys(const unsigned char* receive, const unsigned char* send) override
    {
        std::lock_guard<std::recursive_mutex> lock(receiptMutex);
        if (channel) channel->authentication.setKeys(receive, send, false);
    }

	virtual IntrusivePtr<PacketBuffer> SendPayload(BYTE* buffer, __int32 bufferSize, DWORD& msgID, DeliveryOptions flags, const IntrusivePtr<PacketBuffer>& dependOn);

	virtual void QueryPendingSends(__int32& nMsg, __int32& nBytes, __int32& nMsgG, __int32& nBytesG);

	virtual bool QueryConnectionMetrics(__int32& latencyMS, __int32& throughputBPS);
    void GetTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const override
    {
        std::lock_guard<std::recursive_mutex> guard(poolCriticalSection());
        EndpointInterface* peer = clientEndpoint();
        incoming = outgoing = 0;
        if (peer) peer->getTrafficTotals(incoming, outgoing);
    }
	virtual void GetConnectionLimits(__int32& maxBandwidthPerClient);

	virtual bool GetLocalAddress(in_addr& addr, __int32& port) const;

	virtual bool GetLocalAddress(in_addr& addr) const;

	virtual bool GetDistantAddress(in_addr& addr, __int32& port) const;

	virtual bool GetServerAddress(sockaddr_in& address) const;

	virtual bool HasDisconnected();

	virtual DisconnectReason DisconnectCause();
	virtual std::string GetWhySessionTerminatedStr();

	virtual void DrainIncomingPayloads(ClientPayloadHandler* callback, void* context);

	virtual void ClearIncomingPayloads();

	virtual void ClearSendReceipts();

	virtual unsigned TrimTransportStorage();

};

class EndpointBinding
{
public:
	
	__int32 peerId;

	IntrusivePtr<PacketBuffer> reliableFragmentsHead;

	PacketBuffer* reliableFragmentsTail;

	IntrusivePtr<PacketBuffer> priorityFragmentsHead;

	PacketBuffer* priorityFragmentsTail;

	EndpointBinding()
	{
		peerId = 0;
		reliableFragmentsTail = priorityFragmentsTail = NULL;
	}

	EndpointBinding(const EndpointBinding& from)
	{
		peerId = from.peerId;
		reliableFragmentsHead = from.reliableFragmentsHead;
		reliableFragmentsTail = from.reliableFragmentsTail;
		priorityFragmentsHead = from.priorityFragmentsHead;
		priorityFragmentsTail = from.priorityFragmentsTail;
	}

	bool operator==(const EndpointBinding& sec) const
	{
		return (peerId == sec.peerId);
	}
};

class PeerChannelTable
{
	std::unordered_map<__int32, IntrusivePtr<ChannelInterface>> _users;
	// Non-owning reverse index; updated under the host peer-state mutex.
	std::unordered_map<ChannelInterface*, __int32> _peerIds;

	bool getByIndex(unsigned index, IntrusivePtr<ChannelInterface>& result, __int32* key) const
	{
		if (index >= _users.size())
		{
			result = NULL;
			if (key)
				*key = -1;
			return false;
		}

		std::unordered_map<__int32, IntrusivePtr<ChannelInterface>>::const_iterator it = _users.begin();
		std::advance(it, index);
		result = it->second;
		if (key)
			*key = it->first;
		return true;
	}

public:
	unsigned card() const
	{
		return (unsigned)_users.size();
	}

	bool get(__int32 key, IntrusivePtr<ChannelInterface>& result) const
	{
		std::unordered_map<__int32, IntrusivePtr<ChannelInterface>>::const_iterator it = _users.find(key);
		if (it == _users.end())
		{
			result = NULL;
			return false;
		}
		result = it->second;
		return true;
	}

	bool put(__int32 key, ChannelInterface* value)
	{
		if (!value) return false;
		auto existing = _peerIds.find(value);
		if (existing != _peerIds.end() && existing->second != key) return false;
		auto old = _users.find(key);
		if (old != _users.end()) _peerIds.erase(old->second.GetRef());
		_users[key] = value;
		_peerIds[value] = key;
		return true;
	}

	bool removeKey(__int32 key)
	{
		auto it = _users.find(key);
		if (it == _users.end()) return false;
		_peerIds.erase(it->second.GetRef());
		_users.erase(it);
		return true;
	}

	__int32 peerId(ChannelInterface* channel) const
	{
		auto it = _peerIds.find(channel);
		return it == _peerIds.end() ? -1 : it->second;
	}

	std::vector<__int32> timedOutPeers() const
	{
		std::vector<__int32> result;
		for (const auto& peer : _users)
			if (peer.second->hasTimedOut()) result.push_back(peer.first);
		return result;
	}

	bool getFirst(unsigned& iterator, IntrusivePtr<ChannelInterface>& first, __int32* key = NULL) const
	{
		iterator = 0;
		return getByIndex(iterator++, first, key);
	}

	bool getNext(unsigned& iterator, IntrusivePtr<ChannelInterface>& next, __int32* key = NULL) const
	{
		return getByIndex(iterator++, next, key);
	}
};

class HostSession : public HostSessionInterface
{

protected:
	
	std::string _password;

	RoomAdvertisement session;

	mutable std::recursive_mutex peerStateMutex;

	unsigned short roomListenPort;

	std::string roomDisplayName;

	IncomingPacketQueue alreadyReceived;

	void enqueueIncomingPayload(PacketBuffer* msg);

	std::unordered_map<unsigned __int64, EndpointBinding> peerFragmentState;

	mutable std::recursive_mutex inboundMutex;

	IntrusivePtr<PacketBuffer> sent;

	mutable std::recursive_mutex receiptMutex;

	PeerChannelTable users;


	std::vector<PeerJoinedEvent> pendingJoins;

	std::vector<PeerLeftEvent> pendingDepartures;

	bool acceptConnections;

	struct PlayerChallengeSent {
		sockaddr_in addr;
		__int32 challenge;
		unsigned __int64 time;
	};

	typedef std::vector<PlayerChallengeSent> PlayerChallengeSentList;
	PlayerChallengeSentList _challengesSent;
	double challengeTokens = 128;
	unsigned __int64 challengeTime = GetTickCount64();

	static void Expire(PlayerChallengeSentList& list, unsigned __int64 timeout);
	static __int32 FindChallenge(const PlayerChallengeSentList& list, const sockaddr_in& addr);
	static void DeleteChallengeAt(PlayerChallengeSentList& list, __int32 index);
	static void DeleteAllChallenges(PlayerChallengeSentList& list, const sockaddr_in& addr);

	__int32 findPeerIdForChannel(ChannelInterface* ch);

	friend PacketStatus onHandshakePacket(PacketBuffer* msg, PacketStatus event, void* data);
	friend PacketStatus onHostPacket(PacketBuffer* msg, PacketStatus event, void* data);
	friend PacketStatus onHostSendFinished(PacketBuffer* msg, PacketStatus event, void* data);

	void disconnectRemotePeer(ChannelInterface* ch, DisconnectReason reason, const char* reasonStr = NULL);

	void finalizePeerRemoval(__int32 player);


public:
	
	HostSession();

	virtual ~HostSession();

	virtual bool StartSession(std::string name, std::string password, __int32 port);
	void UpdateLockedOrPassworded(bool lock, bool passworded);

	virtual __int32 ListenPort()
	{
		return roomListenPort;
	}

	virtual std::string RoomName()
	{
		return roomDisplayName;
	}

	virtual IntrusivePtr<PacketBuffer> SendPayload(__int32 to, BYTE* buffer, __int32 bufferSize, DWORD& msgID, DeliveryOptions flags, const IntrusivePtr<PacketBuffer>& dependOn);

    void SetPacketKeys(int player, const unsigned char* receive, const unsigned char* send) override
    {
        std::lock_guard<std::recursive_mutex> lock(peerStateMutex);
        IntrusivePtr<ChannelInterface> channel;
        if (users.get(player, channel)) channel->authentication.setKeys(receive, send, true);
    }

	virtual void DiscardOutbound();


	virtual void QueryPendingSends(__int32 to, __int32& nMsg, __int32& nBytes, __int32& nMsgG, __int32& nBytesG);

	virtual bool QueryConnectionMetrics(__int32 to, __int32& latencyMS, __int32& throughputBPS);
    void GetTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const override;
	virtual void GetConnectionLimits(__int32& maxBandwidthPerClient);

	virtual void SetRoomState(__int32 state);

	virtual bool FormatListenAddress(char* address, DWORD addressLen);

	virtual bool GetServerAddress(sockaddr_in& address);
	virtual bool GetClientAddress(__int32 client, sockaddr_in& address);

	virtual void DisconnectPeer(__int32 player, DisconnectReason reason, const char* reasonStr = NULL);

	virtual void DrainIncomingPayloads(HostPayloadHandler* callback, void* context);

	virtual void ClearIncomingPayloads();

	virtual void ClearSendReceipts();

	virtual void DrainPeerEvents(PeerJoinedHandler* callbackCreate, PeerLeftHandler* callbackDelete, void* context);

	virtual void ClearPeerEvents();

	virtual void NotifyChannelDataSent(__int32 player, size_t size);

	virtual unsigned TrimTransportStorage();

	virtual IntrusivePtr<ChannelInterface> playerToChannel(__int32 player);
};

PacketStatus challengeReceive(PacketBuffer* msg, PacketStatus event, void* data);
PacketStatus onHandshakePacket(PacketBuffer* msg, PacketStatus event, void* data);
PacketStatus onClientPacket(PacketBuffer* msg, PacketStatus event, void* data);
PacketStatus onHostPacket(PacketBuffer* msg, PacketStatus event, void* data);

static IntrusivePtr<EndpointRegistry> registryStorage; 

static IntrusivePtr<EndpointInterface> clientPeer; 

static IntrusivePtr<EndpointInterface> serverPeer; 

void releaseTransportRegistry();

void ensureEndpointRegistry()
{
	if (!registryStorage)
	{
        registryStorage = new EndpointRegistry();
        // Run while all global pools and synchronization objects are still alive.
        static const int registered = std::atexit(releaseTransportRegistry);
        if (registered != 0) Error("Could not register network shutdown");
	}
}

void releaseTransportRegistry()
{
    // Listener callbacks may need the pool mutex; never hold it while joining them.
    IntrusivePtr<EndpointInterface> closingClient, closingServer;
    poolCriticalSection().lock();
    closingClient = clientPeer;
    closingServer = serverPeer;
    poolCriticalSection().unlock();
    if (closingClient) closingClient->stopThreads();
    if (closingServer) closingServer->stopThreads();
    poolCriticalSection().lock();
	if (clientPeer)
	{
		if (registryStorage)
			registryStorage->removeEndpoint(clientPeer.GetRef());
		else
			clientPeer->closeTransport();
		clientPeer = NULL;
	}
	if (serverPeer)
	{
		if (registryStorage)
			registryStorage->removeEndpoint(serverPeer.GetRef());
		else
			serverPeer->closeTransport();
		serverPeer = NULL;
	}

	registryStorage = NULL;
	poolCriticalSection().unlock();
}

static void configureListenPorts(SequenceBitmap& mask, bool server)
{
	mask.empty();
	mask.on(server ? DefaultListenPort() : 0);
}

static EndpointInterface* clientEndpoint()

{
	if (!clientPeer)
	{
		clientPeer = endpointRegistry()->makeEndpoint(NULL);
		if (clientPeer)
		{
			ChannelInterface* ctrl = clientPeer->handshakeChannel();
			if (ctrl)
				ctrl->setReceiveCallback(challengeReceive);
		}
	}
	return clientPeer.GetRef();
}

static EndpointInterface* hostEndpoint(bool create = true)

{
	if (create && !serverPeer)
	{
		serverPeer = endpointRegistry()->makeEndpoint(DefaultListenPort());
		if (serverPeer)
		{
			ChannelInterface* ctrl = serverPeer->handshakeChannel();
			if (ctrl)
				ctrl->setReceiveCallback(onHandshakePacket);
		}
	}
	return serverPeer.GetRef();
}



static ClientSession* _client = NULL;

static HostSession* _server = NULL;

std::recursive_mutex& NatCriticalSection()
{
	static std::recursive_mutex stateMutex;
	return stateMutex;
}

void enterNN() { NatCriticalSection().lock(); }
void leaveNN() { NatCriticalSection().unlock(); }

void stopUdpWorkers()
{
	IntrusivePtr<EndpointInterface> client, server;
	{
		std::lock_guard<std::recursive_mutex> lock(poolCriticalSection());
		client = clientPeer;
		server = serverPeer;
	}
	// Callbacks may acquire the pool mutex while finishing their last packet.
	if (client) client->stopThreads();
	if (server) server->stopThreads();
}

void HostSession::Expire(PlayerChallengeSentList& list, unsigned __int64 timeout)
{
	const unsigned __int64 now = GetTickCount64();
	list.erase(std::remove_if(list.begin(), list.end(), [now, timeout](const PlayerChallengeSent& item) {
		return now - item.time > timeout;
	}), list.end());
}

__int32 HostSession::FindChallenge(const PlayerChallengeSentList& list, const sockaddr_in& addr)
{
	for (size_t i = 0; i < list.size(); ++i)
		if (list[i].addr.sin_port == addr.sin_port && IPV4_HOST_ORDER(list[i].addr) == IPV4_HOST_ORDER(addr))
			return (__int32)i;
	return -1;
}

void HostSession::DeleteChallengeAt(PlayerChallengeSentList& list, __int32 index)
{
	if (index >= 0 && (size_t)index < list.size())
		list.erase(list.begin() + index);
}

void HostSession::DeleteAllChallenges(PlayerChallengeSentList& list, const sockaddr_in& addr)
{
	list.erase(std::remove_if(list.begin(), list.end(), [&addr](const PlayerChallengeSent& item) {
		return item.addr.sin_port == addr.sin_port && IPV4_HOST_ORDER(item.addr) == IPV4_HOST_ORDER(addr);
	}), list.end());
}

PacketStatus onHandshakePacket(PacketBuffer* msg, PacketStatus event, void* data)
{
	if (!_server || !msg || msg->payloadLength() < 4 ||
		!(msg->packetFlags() & PACKET_HANDSHAKE))
		return PacketNoMoreCallbacks; 

	unsigned __int32 magic = *(unsigned __int32*)msg->payloadBytes();
	struct sockaddr_in distant;
	msg->packetDestination(distant);

	switch (magic)
	{

	case JOIN_PROBE_TAG:
		if (_server->acceptConnections && msg->payloadLength() == sizeof(JOIN_PROBE_TAG))
		{
			_server->peerStateMutex.lock(); 
			const auto now = GetTickCount64();
			_server->challengeTokens = (std::min)(128.0, _server->challengeTokens + (now - _server->challengeTime) * 0.128);
			_server->challengeTime = now;
			if (_server->challengeTokens < 1)
			{
				_server->peerStateMutex.unlock();
				break;
			}
			_server->challengeTokens -= 1;
			_server->Expire(_server->_challengesSent, JOIN_REPLY_TIMEOUT_MS);
			__int32 hasBeenSent = HostSession::FindChallenge(_server->_challengesSent, distant);
			if (hasBeenSent < 0)
			{
				if (_server->_challengesSent.size() >= 1024)
				{
					_server->peerStateMutex.unlock();
					break;
				}
				HostSession::PlayerChallengeSent sent;
				sent.addr = distant;
				sent.challenge = GenerateServerChallenge(distant);
				_server->_challengesSent.push_back(sent);
				hasBeenSent = (__int32)_server->_challengesSent.size() - 1;
			}

			JoinChallenge chp;
			chp.magic = JOIN_CHALLENGE_TAG;
			chp.challenge = _server->_challengesSent[hasBeenSent].challenge;
			_server->_challengesSent[hasBeenSent].time = now;

			_server->Expire(_server->_challengesSent, JOIN_REPLY_TIMEOUT_MS);

			_server->peerStateMutex.unlock();


			IntrusivePtr<PacketBuffer> out = PacketCache::sharedPacketCache()->acquirePacket(sizeof(chp), msg->ownerChannel());
			if (out)
			{
				out->assignDestination(distant);
				out->updatePacketFlags(PACKET_FLAG_MASK, PACKET_TO_CONTROL_CHANNEL | PACKET_HANDSHAKE);
				out->assignPayload((unsigned char*)&chp, sizeof(chp));
				out->queueForSend(true); 
			}

		}
		break;

	case JOIN_RESPONSE_TAG:
		if (_server->acceptConnections &&
			(msg->payloadLength() == sizeof(JoinRequest) || msg->payloadLength() == sizeof(VerifiedJoinRequest)))
		{
			JoinRequest* cpp = (JoinRequest*)msg->payloadBytes();
			_server->peerStateMutex.lock(); 

			if (magic == JOIN_RESPONSE_TAG && msg->payloadLength() == sizeof(VerifiedJoinRequest))
			{
				__int32 challenge = ((VerifiedJoinRequest*)cpp)->challenge;
				__int32 hasBeenSent = HostSession::FindChallenge(_server->_challengesSent, distant);
				if (hasBeenSent < 0)
				{
					Error("Server: Challenge %x not found", challenge);
					_server->peerStateMutex.unlock();
					break;
				}
				else if (challenge != _server->_challengesSent[hasBeenSent].challenge)
				{
					Error("Server: Challenge not matching %x!=%x", challenge, _server->_challengesSent[hasBeenSent].challenge);
					_server->peerStateMutex.unlock();
					break;
				}
				else
				{
					HostSession::DeleteChallengeAt(_server->_challengesSent, hasBeenSent);
				}
			}
			else
			{
				_server->peerStateMutex.unlock();
				break;
			}

			HostSession::DeleteAllChallenges(_server->_challengesSent, distant);

			JoinResult result = JoinOK;

			__int32 player = 0; 
			ChannelInterface* ch = NULL;
			if (result == JoinOK)
			{
				poolCriticalSection().lock();
				EndpointInterface* peer = hostEndpoint();
				IntrusivePtr<ChannelInterface> findCh;
				ch = peer->lookupChannel(distant);
				if (ch)
				{ 
					unsigned it;
					__int32 existingPlayer = -1;
					IntrusivePtr<ChannelInterface> existingCh;
					if (_server->users.getFirst(it, existingCh, &existingPlayer))
					{
						do
						{
							if (existingCh.GetRef() == ch)
								break;
						}
						while (_server->users.getNext(it, existingCh, &existingPlayer));
					}
					if (existingPlayer >= RESERVED_PEER_ID_COUNT && existingCh.GetRef() == ch)
					{ 
						poolCriticalSection().unlock();
						_server->peerStateMutex.unlock();
						break; 
					}
					
					_server->finalizePeerRemoval(_server->findPeerIdForChannel(ch));
					ch = NULL;
					poolCriticalSection().unlock();
					_server->peerStateMutex.unlock();
					Sleep(PEER_REMOVAL_WAIT_MS); 
					_server->peerStateMutex.lock();
					poolCriticalSection().lock();
				}
				
				player = RESERVED_PEER_ID_COUNT;
				while (_server->users.get(player, findCh))
					player++;

				__int32 publicPlayers = _server->users.card() + 1; 
				if (_server->session.roomCapacity > 0 &&
						publicPlayers >= _server->session.roomCapacity)
					result = JoinSessionFull;
				else
				{ 
					ch = peer->endpointRegistry()->makeChannel(distant, peer);
					if (!ch)
					{
						result = JoinError; 
						Error("HostSession: createChannel() failed => cannot insert a new player");
					}
					else
					{ 

						_server->users.put(player, ch);
						EndpointBinding sup;
						sup.peerId = player;
						{ std::lock_guard<std::recursive_mutex> lock(_server->inboundMutex); _server->peerFragmentState[udpEndpointKey(distant)] = sup; }
						_server->pendingJoins.push_back(PeerJoinedEvent());
						PeerJoinedEvent& info = _server->pendingJoins.back();
						info.player = player;

						struct sockaddr_in inaddr;
						msg->packetDestination(inaddr);
						info.inaddr = inaddr.sin_addr.s_addr; 

						// Remote peers cannot claim privileged transport roles.
						info.reservedBotRole = false;
						strncpy(info.name, cpp->name, sizeof(info.name));
						info.name[sizeof(info.name) - 1] = (char)0;
						if (memcmp(cpp->name, info.name, sizeof(info.name)))
							Error("HostSession: name of a new player is too long => truncating to '%s'", info.name);
						ch->setReceiveCallback(onHostPacket);
					}
				}
				poolCriticalSection().unlock();
			}
			_server->peerStateMutex.unlock();
			
			JoinReply app;
			app.magic = JOIN_ACCEPT_TAG;
			app.result = result;
			app.assignedPeerId = player; 
								   
			IntrusivePtr<PacketBuffer> out = PacketCache::sharedPacketCache()->acquirePacket(sizeof(JoinReply), ch ? ch : msg->ownerChannel());
			if (out)
			{
				if (!ch)
					out->assignDestination(distant);
				out->updatePacketFlags(PACKET_FLAG_MASK, PACKET_HANDSHAKE | (ch ? PACKET_RELIABLE : PACKET_FROM_CONTROL_CHANNEL));
				out->assignPayload((unsigned char*)&app, sizeof(app));
				out->queueForSend(true); 
			}
			if (ch) 
				ch->updateLiveness(0);
		}
		break;


	}

	return PacketNoMoreCallbacks;
}

PacketStatus challengeReceive(PacketBuffer* msg, PacketStatus event, void* data)
{
	
	if (!_client ||
		!msg || msg->payloadLength() < 4 || !(msg->packetFlags() & PACKET_HANDSHAKE))
		return PacketNoMoreCallbacks; 

	unsigned __int32 magic = *(unsigned __int32*)msg->payloadBytes();
	struct sockaddr_in distant;
	msg->packetDestination(distant);
	unsigned __int32 ip = ntohl(distant.sin_addr.s_addr);
	unsigned short port = ntohs(distant.sin_port);

	switch (magic)
	{
	case JOIN_CHALLENGE_TAG:
		if (msg->payloadLength() == sizeof(JoinChallenge) && _client)
		{
			JoinChallenge* chp = (JoinChallenge*)msg->payloadBytes();
			_client->receiptMutex.lock();
			if (_client->challengePlayer == 0 &&
				udpEndpointKey(distant) == udpEndpointKey(_client->connectingAddress))
			{
				_client->challengePlayer = chp->challenge;
			}
			_client->receiptMutex.unlock();
		}
		break; 
	}

	return PacketNoMoreCallbacks;
}

bool ClientSession::SendMagicPacket(const void* data, size_t size)
{
	if (size > 0xffffffffu)
	{
		return false;
	}
	IntrusivePtr<PacketBuffer> msgReq = PacketCache::sharedPacketCache()->acquirePacket(static_cast<unsigned>(size), channel.GetRef());
	if (!msgReq)
	{
		return false;
	}
	msgReq->updatePacketFlags(PACKET_FLAG_MASK, PACKET_TO_CONTROL_CHANNEL | PACKET_HANDSHAKE);
	msgReq->assignPayload((const unsigned char*)data, static_cast<unsigned __int32>(size));
	msgReq->queueForSend(true); 
	return true;
}

JoinResult ClientSession::StartSession(std::string address, std::string password, bool reservedBotRole, unsigned short& port,
    std::string player, JoinCancellationCheck* cancelNNCallback)
{
    if (channel || connectResult == JoinNone) return JoinError;
    std::string ip;
    if (!parseServerAddress(address.empty() ? "127.0.0.1" : address, ip, port)) return JoinError;
    connectingAddress.sin_family = AF_INET;
    connectingAddress.sin_port = htons(port);
    resolving = InetPtonA(AF_INET, ip.c_str(), &connectingAddress.sin_addr) != 1;
    connectDeadline = GetTickCount64() + JOIN_REPLY_TIMEOUT_MS;
    nextConnectSend = 0;
    challengeSent = false;
    cancelConnect = cancelNNCallback;
    connectPacket = {};
    connectPacket.magic = JOIN_RESPONSE_TAG;
    strncpy(connectPacket.name, player.c_str(), PLAYER_NAME_LEN - 1);
    strncpy(connectPacket.password, password.c_str(), PASSWORD_LEN - 1);
    connectPacket.reservedBotRole = reservedBotRole ? 1 : 0;
    localBotRole = reservedBotRole;
    joinReplyCode = JoinNone;
    challengePlayer = 0;
    sessionClosed = false;
    sessionCloseReason = DisconnectOther;
    connectResult = JoinNone;
    if (resolving) resolver.start(ip);
    return JoinNone;
}

JoinResult ClientSession::PollJoin()
{
    if (connectResult != JoinNone) return connectResult;
    const unsigned __int64 now = GetTickCount64();
    if (now >= connectDeadline || (cancelConnect && cancelConnect()))
    {
        resolver.cancel();
        return connectResult = JoinTimeout;
    }
    if (resolving)
    {
        int result = resolver.poll(connectingAddress);
        if (!result) return JoinNone;
        if (result < 0) return connectResult = JoinError;
        resolving = false;
    }
    enterAll();
    if (!channel)
    {
        channel = endpointRegistry()->makeChannel(connectingAddress, clientEndpoint());
        if (!channel) { leaveAll(); return connectResult = JoinError; }
        channel->setReceiveCallback(onClientPacket, channel.GetRef());
    }
    if (joinReplyCode != JoinNone)
    {
        channel->updateLiveness(0);
        connectResult = static_cast<JoinResult>(joinReplyCode);
    }
    else if (now >= nextConnectSend || (!challengeSent && challengePlayer != 0))
    {
        bool sent;
        if (!challengePlayer)
        {
            unsigned __int32 request = JOIN_PROBE_TAG;
            sent = SendMagicPacket(&request, sizeof(request));
        }
        else
        {
            challengeSent = true;
            connectPacket.challenge = challengePlayer;
            sent = SendMagicPacket(&connectPacket, sizeof(connectPacket));
        }
        if (!sent) connectResult = JoinError;
        nextConnectSend = now + JOIN_RETRY_INTERVAL_MS;
    }
    leaveAll();
    return connectResult;
}

void VoiceDataSent(__int32 pid, __int32 len)
{
	if (_server)
		_server->NotifyChannelDataSent(pid, len);
}

void HostSession::NotifyChannelDataSent(__int32 player, size_t size)
{
	IntrusivePtr<ChannelInterface> channel;
	users.get(player, channel);
	if (channel)
	{
		channel->dataSentAck(size);
	}
}

#include "SessionOperations.hpp"
