#ifdef _MSC_VER
#pragma once
#endif

#ifndef NET_TRANSPORT_HPP
#define NET_TRANSPORT_HPP

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <string>
#include <vector>

#include "IntrusivePtr.h"
#include "SequenceBitmap.hpp"

#include "TransportIncludes.hpp"

#ifndef DECL_ENUM_CONNECT_RESULT
#define DECL_ENUM_CONNECT_RESULT
enum JoinResult : __int32;
#endif
enum JoinResult : __int32
{
	JoinNone = -1,
	JoinOK,
	JoinPassword,
	JoinVersion,
	JoinError,
	JoinName,
	JoinSessionFull,
	JoinTimeout,
};

enum DeliveryOptions {
	DeliveryNone = 0,
	DeliveryGuaranteed = 1,
	DeliveryHighPriority = 2,
	DeliveryStatsAlreadyDone = 4,
	DeliverySetCallback = 8
};

enum ChatPacketKind : unsigned char {
    NAMTConnect = 1, NAMTDisconnect = 2, NAMTChat = 3, NAMTHeartbeat = 4,
    NAMTPlayerAssign = 5, NAMTVoice = 12, NAMTKeyHello = 13, NAMTKeyAccept = 14,
    NAMTChatKey = 17, NAMTPrivateChat = 18, NAMTEncrypted = 26, NAMTSessionEnd = 27, NAMTPresence = 28, NAMTCommandError = 29, NAMTSystemNotice = 30, NAMTFile = 31, NAMTScreen = 32, NAMTUpdate = 33, NAMTResumeHello = 34, NAMTResumeAccept = 35, NAMTHistory = 36
};

#pragma pack(push, netAppMessage, 1)
struct ChatPacketPrefix {
	ChatPacketKind type;
};
#pragma pack(pop, netAppMessage)

enum DisconnectReason
{
	DisconnectTimeout,
	DisconnectDisconnected,
	DisconnectKicked,
	DisconnectBanned,
	DisconnectSessionLocked,
	DisconnectOther, 
};

__forceinline DeliveryOptions operator|(DeliveryOptions a, DeliveryOptions b)
{
	return DeliveryOptions((__int32)a | (__int32)b);
}

struct SendReceipt {
	SendReceipt() : msgID(0), ok(false) {}

	DWORD msgID;
	bool ok;
};
struct PeerJoinedEvent {
	PeerJoinedEvent() : player(0), reservedBotRole(false), inaddr(0)
	{
		name[0] = 0;
	}

	__int32 player;
	bool reservedBotRole;
	unsigned long inaddr;
	char name[40];
};
struct PeerLeftEvent {
	PeerLeftEvent() : player(0) {}

	__int32 player;
};
typedef void ClientPayloadHandler(char* buffer, __int32 bufferSize, void* context);
typedef void HostPayloadHandler(__int32 from, char* buffer, __int32 bufferSize, void* context);
typedef void SendReceiptHandler(DWORD msgID, bool ok, void* context);
typedef void PeerJoinedHandler(__int32 player, bool reservedBotRole, const char* name, unsigned long inaddr, void* context);
typedef void PeerLeftHandler(__int32 player, void* context);

typedef bool JoinCancellationCheck();

class ClientSessionInterface
{
public:
	
	ClientSessionInterface() {}
	
	virtual ~ClientSessionInterface() {}

	virtual JoinResult StartSession(
		std::string address, std::string password, bool reservedBotRole, unsigned short& port,
		std::string player, JoinCancellationCheck* cancelNNCallback = NULL) = 0;

	// StartSession starts an attempt; poll without blocking until the result is not JoinNone.
	virtual JoinResult PollJoin() = 0;
    virtual void SetPacketKeys(const unsigned char* receive, const unsigned char* send) = 0;

	virtual IntrusivePtr<PacketBuffer> SendPayload(BYTE* buffer, __int32 bufferSize, DWORD& msgID, DeliveryOptions flags, const IntrusivePtr<PacketBuffer>& dependOn) = 0;
	virtual void QueryPendingSends(__int32& nMsg, __int32& nBytes, __int32& nMsgG, __int32& nBytesG) = 0;
	virtual bool QueryConnectionMetrics(__int32& latencyMS, __int32& throughputBPS) = 0;
    virtual void GetTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const = 0;
	virtual bool QueryRawConnectionMetrics(__int32& latencyMS, __int32& throughputBPS)
	{
		return QueryConnectionMetrics(latencyMS, throughputBPS);
	}

	virtual bool GetLocalAddress(in_addr& addr) const { return false; }
	virtual bool GetLocalAddress(in_addr& addr, __int32& port) const { return false; }
	virtual bool GetDistantAddress(in_addr& addr, __int32& port) const { return false; }

	virtual bool GetServerAddress(sockaddr_in& addr) const { return false; }

	virtual bool HasDisconnected() = 0;
	virtual DisconnectReason DisconnectCause() = 0;
	virtual std::string GetWhySessionTerminatedStr() = 0;

	virtual void DrainIncomingPayloads(ClientPayloadHandler* callback, void* context) = 0;
	virtual void ClearIncomingPayloads() = 0;
	virtual void ClearSendReceipts() = 0;

	virtual unsigned TrimTransportStorage()
	{
		return 0;
	}
};

class HostSessionInterface
{
public:
	
	HostSessionInterface() {}
	
	virtual ~HostSessionInterface() {}

	virtual bool StartSession(std::string name, std::string password, __int32 port) = 0;
	
	virtual void UpdateLockedOrPassworded(bool lock, bool passworded) = 0;
	
	virtual std::string RoomName() = 0;
	
	virtual __int32 ListenPort() = 0;

	virtual IntrusivePtr<PacketBuffer> SendPayload(__int32 to, BYTE* buffer, __int32 bufferSize, DWORD& msgID, DeliveryOptions flags, const IntrusivePtr<PacketBuffer>& dependOn) = 0;
    virtual void SetPacketKeys(int player, const unsigned char* receive, const unsigned char* send) = 0;
	virtual void DiscardOutbound() = 0;
	virtual void QueryPendingSends(__int32 to, __int32& nMsg, __int32& nBytes, __int32& nMsgG, __int32& nBytesG) = 0;
	virtual bool QueryConnectionMetrics(__int32 to, __int32& latencyMS, __int32& throughputBPS) = 0;
    virtual void GetTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const = 0;
	virtual bool QueryRawConnectionMetrics(__int32 to, __int32& latencyMS, __int32& throughputBPS)
	{
		return QueryConnectionMetrics(to, latencyMS, throughputBPS);
	}
	virtual void GetConnectionLimits(__int32& maxBandwidthPerClient)
	{
		maxBandwidthPerClient = INT_MAX;
	}
	
	virtual void SetRoomState(__int32 state) = 0;
	virtual void DisconnectPeer(__int32 player, DisconnectReason reason, const char* reasonStr = NULL) = 0;
	
	virtual bool FormatListenAddress(char* address, DWORD addressLen) = 0;

	virtual bool GetServerAddress(sockaddr_in& address) = 0;
	virtual bool GetClientAddress(__int32 client, sockaddr_in& address) = 0;

	virtual void DrainIncomingPayloads(HostPayloadHandler* callback, void* context) = 0;
	virtual void ClearIncomingPayloads() = 0;
	virtual void ClearSendReceipts() = 0;
	virtual void DrainPeerEvents(PeerJoinedHandler* callbackCreate, PeerLeftHandler* callbackDelete, void* context) = 0;
	virtual void ClearPeerEvents() = 0;

	virtual unsigned TrimTransportStorage()
	{
		return 0;
	}
};



bool parseServerAddress(const std::string& address, std::string& host, unsigned short& port);

ClientSessionInterface* MakeClientSession();

HostSessionInterface* MakeHostSession(unsigned short port = 777);

void stopUdpWorkers();
void releaseTransportRegistry();

void enterNN();
void leaveNN();

#endif
