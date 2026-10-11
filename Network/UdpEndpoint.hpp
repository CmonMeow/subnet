#ifdef _MSC_VER
#pragma once
#endif

#ifndef NETPEER_H
#define NETPEER_H

#include <winsock2.h>
#include <unordered_map>
#include <atomic>
#include "SequenceBitmap.hpp"

extern unsigned long bindIPAddress;

const __int32 AckRateSampleSlots = 64;
const __int32 SendRateSampleSlots = 256;

inline bool localEndpointAddress(struct sockaddr_in& me, unsigned short port)
{
	me.sin_family = AF_INET;
	char meName[128];
	if (gethostname(meName, 128) == SOCKET_ERROR)
		return false;

	struct hostent* h = gethostbyname(meName);
	if (h)
	{
		if (h->h_length < 4)
			return false;
		memcpy(&(me.sin_addr.s_addr), h->h_addr, 4);
	}
	else
	{
		me.sin_addr.s_addr = inet_addr(meName);
		if (me.sin_addr.s_addr == INADDR_NONE)
			return false;
	}
	me.sin_port = htons(port);
	return true;
}

inline bool resolveLocalHostAddress(struct sockaddr_in& host, const char* ip, unsigned short port)
{
	host.sin_family = AF_INET;
	host.sin_port = htons(port);
	if (!ip || !ip[0])
		host.sin_addr.s_addr = INADDR_BROADCAST;
	else
	{
		host.sin_addr.s_addr = inet_addr(ip);
		if (host.sin_addr.s_addr == INADDR_NONE)
		{
			struct hostent* h = gethostbyname(ip);
			if (!h)
				return false;
			host.sin_addr.s_addr = *(unsigned __int32*)h->h_addr_list[0];
		}
	}
	return true;
}

unsigned __int32 crc32(unsigned __int32 crc, const unsigned char* buf, __int64 len);

class UdpEndpoint : public EndpointInterface
{

protected:
	
	std::unordered_map<unsigned __int64, IntrusivePtr<ChannelInterface>> chMap;

    std::atomic<unsigned __int64> incomingBytes{0};
    std::atomic<unsigned __int64> outgoingBytes{0};
	SOCKET sock;

	HANDLE listener = NULL;

	std::atomic<bool> listen{false};

	friend DWORD WINAPI runUdpWorker(void* param);

	void reopenEndpoint();

	bool reconnecting;

public:
    void getTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const override
    {
        incoming = incomingBytes.load(std::memory_order_relaxed);
        outgoing = outgoingBytes.load(std::memory_order_relaxed);
    }

	UdpEndpoint(EndpointRegistry* _pool) : EndpointInterface(_pool)
	{
		sock = INVALID_SOCKET;
		port = 0;
		listen = reconnecting = false;
	}

	UdpEndpoint(SOCKET _sock, unsigned short _port, EndpointRegistry* _pool)
		: EndpointInterface(_pool)
	{
		stateMutex.lock();
		sock = _sock;
		port = _port;
		listen = reconnecting = false;
		handshakeLink = NULL;
		if (registryStorage)
		{ 
			handshakeLink = registryStorage->makeChannel(true);
			if (handshakeLink)
			{
				struct sockaddr_in distant;
				memset((void*)&(distant), NULL, sizeof(distant));
				distant.sin_addr.s_addr = INADDR_BROADCAST;
				handshakeLink->openChannel(this, distant);
			}
		}
		if (sock != INVALID_SOCKET)
		{ 
			
			listen = true;
			DWORD thid;
			listener = CreateThread(NULL, 32 * 1024, &runUdpWorker, this, 0, &thid);
			if (listener)
				SetThreadPriority(listener, THREAD_PRIORITY_HIGHEST); 
			else
				listen = false; 
		}
		stateMutex.unlock();
	}

	virtual void localEndpointAddress(struct sockaddr_in& local) const { ::localEndpointAddress(local, port); }

	virtual SOCKET GetSocket() const { return sock; }

	virtual bool attachChannel(struct sockaddr_in& distant, ChannelInterface* ch)
	{
		if (!ch)
			return false;
		stateMutex.lock();
		const unsigned __int64 key = udpEndpointKey(distant);
		bool result = (chMap.find(key) == chMap.end());
		if (result)
			chMap[key] = ch;
		stateMutex.unlock();
		return result;
	}

	virtual void detachChannel(ChannelInterface* ch)
	{
        if (!ch) return;
        stateMutex.lock();
        for (auto it = chMap.begin(); it != chMap.end();)
		{
			if (it->second.GetRef() == ch)
				it = chMap.erase(it);
			else
				++it;
		}
        stateMutex.unlock();
	}

	virtual ChannelInterface* lookupChannel(const struct sockaddr_in& distant);

	virtual void closeTransport();

	virtual void stopThreads()
	{
		stateMutex.lock();
		bool wasListen = listen;
		listen = false;
		stateMutex.unlock();
		if (wasListen)
		{
			if (listener)
				WaitForSingleObject(listener, INFINITE);
			CloseHandle(listener);
		}
	}

	virtual void handleDatagram(DatagramHeader* hdr, const struct sockaddr_in& distant) {}

	virtual PacketStatus transmitDatagram(DatagramHeader* hdr, struct sockaddr_in distant)
	{
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (sock != INVALID_SOCKET)
		{
            if (hdr->length < sizeof(DatagramHeader) || hdr->length > DatagramReceiveBufferBytes) return PacketError;
			hdr->crc = 0;
			hdr->crc = crc32(0, (const unsigned char*)hdr, hdr->length);

            unsigned char packet[DatagramReceiveBufferBytes + DatagramAuthentication::TagBytes];
            memcpy(packet, hdr, hdr->length);
            size_t packetBytes = hdr->length;
            const auto channel = chMap.find(udpEndpointKey(distant));
            if (channel != chMap.end()) packetBytes = channel->second->authentication.sign(packet, packetBytes);
			char retryCounter = 12;
		retry:
			if (sendto(sock, reinterpret_cast<const char*>(packet), (int)packetBytes, 0, (const sockaddr*)&distant, sizeof(distant)) != SOCKET_ERROR)
			{
                outgoingBytes.fetch_add(packetBytes, std::memory_order_relaxed);
				return PacketOutputSent;
			}
			__int32 werror = WSAGetLastError();
			__int32 error = 0;
			__int32 errLen = sizeof(error);
			getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &errLen);

			WSASetLastError(0); 

			if (werror == WSAECONNRESET)
			{ 
				reopenEndpoint();
				if ((sock != INVALID_SOCKET) && (retryCounter--))
					goto retry;
			}
		}
		return PacketError;
	}

	virtual void sendRaw(const sockaddr_in& ia, const void* data, __int32 size, __int32 sizeEncrypted)
	{
		stateMutex.lock(); 
        int sentBytes = sendto(sock, reinterpret_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&ia), sizeof(ia));
        if (sentBytes > 0) outgoingBytes.fetch_add(sentBytes, std::memory_order_relaxed);
		stateMutex.unlock();
	}

	virtual void discardPendingPackets()
	{
		stateMutex.lock();
        const auto channels = chMap;
        for (const auto& it : channels)
            if (it.second) it.second->discardPendingPackets();

		stateMutex.unlock();
	}

	virtual ~UdpEndpoint() { closeTransport(); }
};

#endif
