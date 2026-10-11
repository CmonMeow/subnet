#ifdef _MSC_VER
#pragma once
#endif

#ifndef NETAPI_H
#define NETAPI_H

#include <unordered_map>
#include <mutex>
#include "DatagramAuthentication.hpp"
#include <vector>
#include "IntrusivePtr.h"

class EndpointInterface;
class ChannelInterface;
class EndpointRegistry;
class PacketBuffer;

// Absorb simultaneous file chunks and voice from a room of senders.
constexpr int SocketReceiveBufferBytes = 1024 * 1024;

struct TransportTuning {
	unsigned ackTimeoutPaddingMs;					
	unsigned ackRepeatCount;					
	unsigned initialSendRateBytesPerSecond;					
	unsigned minimumSendRateBytesPerSecond;					
	unsigned maximumSendRateBytesPerSecond;					
	unsigned retransmitHistoryMs;						
	unsigned ackRateWindowMs;						
	unsigned receiveSequenceWindowSize;				
	unsigned pendingAckWindowSize;				
};
extern TransportTuning transportTuning;

struct PacketRoute {

	unsigned short andFlag;

	unsigned short eqFlag;

	PacketCallback* receiveCallback;

	void* data;
};

class ChannelInterface : public AtomicRefCount
{

protected:
	
	mutable std::recursive_mutex stateMutex;

	PacketCallback* receiveCallback;

	void* data;

	std::vector<PacketRoute> subsets;

	ChannelInterface()
	{
		receiveCallback = NULL;
		data = NULL;
	}

public:
    DatagramAuthentication authentication;
	virtual PacketStatus openChannel(EndpointInterface* _peer, struct sockaddr_in& distant) = 0;


	virtual void remoteEndpointAddress(struct sockaddr_in& distant) const = 0;

	virtual void localEndpointAddress(struct sockaddr_in& local) const = 0;

	virtual bool dropPacketToSend() { return false; }

	virtual bool isHandshakeChannel() const { return false; }

	virtual unsigned roundTripTime() = 0;

	virtual unsigned estimatedSendRate() = 0;

	virtual unsigned __int64 lastReceiveTime() const = 0;

	virtual void queryPacketBacklog(__int32& msgs, __int32& bytes, __int32& vimMsgs, __int32& vimBytes) = 0;

	virtual void handleDatagram(DatagramHeader* hdr, const struct sockaddr_in& distant) = 0;

	virtual void setReceiveCallback(PacketCallback* processR, void* _data = NULL)
	{
		receiveCallback = processR;
		data = _data;
	}

	virtual void dataSentAck(size_t size) {}

	virtual void setSubsetRoutine(const PacketRoute& item)
	{
		stateMutex.lock();
		removeSubsetRoutine(item);
		subsets.push_back(item);
		stateMutex.unlock();
	}

	virtual bool removeSubsetRoutine(const PacketRoute& item)
	{
		stateMutex.lock();
		__int32 i;
		for (i = 0; i < subsets.size(); i++)
			if (subsets[i].andFlag == item.andFlag && subsets[i].eqFlag == item.eqFlag)
			{
				subsets.erase(subsets.begin() + i);
				stateMutex.unlock();
				return true;
			}
		stateMutex.unlock();
		return false;
	}

	virtual void queueOutboundPacket(PacketBuffer* msg, bool urgent = false) = 0;

	IntrusivePtr<PacketBuffer> prepared;

	virtual PacketBuffer* lastReliablePacket(bool urgent) = 0;

	virtual bool prepareNextPacket() = 0;

	virtual void sendRaw(const sockaddr_in& ia, const void* data, __int32 size, __int32 sizeEncrypted) = 0;

	virtual unsigned __int64 beginSendBatch(unsigned __int64 bunchStart) = 0;

	virtual void finishSendBatch() = 0;

	virtual unsigned __int64 packetSendTime(unsigned __int64 ser) = 0;

	virtual void discardPendingPackets() = 0;

	virtual void updateLiveness(unsigned __int64 now) = 0;

	virtual bool hasTimedOut() = 0;

	virtual void serviceChannel() = 0;

	virtual void closeTransport() = 0;

	virtual ~ChannelInterface() {}
};

class EndpointInterface : public AtomicRefCount
{
protected:
	
	mutable std::recursive_mutex stateMutex;

	EndpointRegistry* registryStorage;
	unsigned short port;
	
	IntrusivePtr<ChannelInterface> handshakeLink;

	EndpointInterface(EndpointRegistry* _pool)
	{
		registryStorage = _pool;
		port = 0;
		handshakeLink = NULL;
	}

public:
	virtual EndpointRegistry* endpointRegistry() const { return registryStorage; }
    // UDP payload bytes, including transport headers but excluding IP/UDP headers.
    virtual void getTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const
    { incoming = outgoing = 0; }

	virtual unsigned short endpointPort() const { return port; }

	virtual void localEndpointAddress(struct sockaddr_in& local) const = 0;

	virtual SOCKET GetSocket() const = 0;

	virtual ChannelInterface* handshakeChannel() const { return handshakeLink.GetRef(); }

	virtual bool attachChannel(struct sockaddr_in& distant, ChannelInterface* ch) = 0;

	virtual void detachChannel(ChannelInterface* ch) = 0;

	virtual ChannelInterface* lookupChannel(const struct sockaddr_in& distant) = 0;

	virtual unsigned short boundUdpPort() const { return port; }

	virtual void closeTransport() = 0;

	virtual void stopThreads() {}

	virtual void suspendSocket(bool susp = true) {}

	virtual void replaceSocket(SOCKET _sock) {}

	virtual void handleDatagram(DatagramHeader* hdr, const struct sockaddr_in& distant) = 0;

	virtual PacketStatus transmitDatagram(DatagramHeader* hdr, struct sockaddr_in distant) = 0;

	virtual void sendRaw(const sockaddr_in& ia, const void* data, __int32 size, __int32 sizeEncrypted) = 0;

	virtual void discardPendingPackets() = 0;

	virtual ~EndpointInterface() { port = 0; }

	void lockEndpoint() { stateMutex.lock(); }
	void unlockEndpoint() { stateMutex.unlock(); }
};

inline unsigned __int64 udpEndpointKey(const struct sockaddr_in& addr)
{
	return ((unsigned __int64)IPV4_HOST_ORDER(addr) | ((unsigned __int64)UDP_PORT_HOST_ORDER(addr) << 32));
}

extern unsigned __int64 channelEndpointKey(const IntrusivePtr<ChannelInterface>& ch);

#include <winsock.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "wsock32")

extern unsigned long bindIPAddress;

class EndpointRegistry : public AtomicRefCount
{
protected:

	std::unordered_map<unsigned short, IntrusivePtr<EndpointInterface>> peers;
	std::unordered_map<unsigned __int64, IntrusivePtr<ChannelInterface>> channels;

public:
	EndpointRegistry()
	{
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
	}

	virtual EndpointInterface* makeEndpoint(unsigned short port);

	virtual ChannelInterface* makeChannel(bool control);

	virtual void removeEndpoint(EndpointInterface* peer)
	{
		if (!peer)
			return;
		peer->closeTransport();
		for (auto it = peers.begin(); it != peers.end();)
		{
			if (it->second.GetRef() == peer)
				it = peers.erase(it);
			else
				++it;
		}
	}

	virtual ChannelInterface* makeChannel(struct sockaddr_in& distant, EndpointInterface* peer = NULL)
	{
		IntrusivePtr<EndpointInterface> pee = peer;
		if (!pee && !peers.empty())
			pee = peers.begin()->second;
		if (!pee)
			return NULL; 
		ChannelInterface* ch = makeChannel(false);
		if (!ch)
			return NULL;
		if (ch->openChannel(pee.GetRef(), distant) != PacketOK)
		{
			delete ch;
			return NULL;
		}
		channels[channelEndpointKey(ch)] = ch;
		return ch;
	}

	virtual ChannelInterface* lookupChannel(struct sockaddr_in& distant)
	{
		auto it = channels.find(udpEndpointKey(distant));
		if (it == channels.end())
			return NULL;
		return it->second.GetRef();
	}

	virtual ChannelInterface* handshakeChannel()
	{
		if (peers.empty())
			return NULL;
		return peers.begin()->second->handshakeChannel();
	}

	virtual void removeChannel(ChannelInterface* channel)
	{
		if (!channel)
			return;
        IntrusivePtr<ChannelInterface> keepAlive = channel;
        channel->closeTransport();
        for (auto it = channels.begin(); it != channels.end();)
		{
			if (it->second.GetRef() == channel)
				it = channels.erase(it);
			else
				++it;
		}
	}

	virtual ~EndpointRegistry() { WSACleanup(); }
};

#endif
