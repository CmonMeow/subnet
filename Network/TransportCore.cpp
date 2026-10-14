

#include "TransportIncludes.hpp"
#include "UdpEndpoint.hpp"
#include "ReliableChannel.hpp"

unsigned __int64 channelEndpointKey(const IntrusivePtr<ChannelInterface>& ch)
{
	struct sockaddr_in addr;
	ch->remoteEndpointAddress(addr);
	return udpEndpointKey(addr);
}

const TransportTuning defaultTransportTuning = {
	400,
	2,		  
	32000,	  
	8192,	  
	2000000,  
	3000,
	3000,
	65536,	  
	400,	  
};
TransportTuning transportTuning = defaultTransportTuning;

EndpointInterface* EndpointRegistry::makeEndpoint(unsigned short port)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
	{
		return NULL;
	}

	DWORD enableReporting = FALSE;
	DWORD returned = 0;
	const DWORD SIO_UDP_CONNRESET = 0x9800000C;
	if (WSAIoctl(s, SIO_UDP_CONNRESET, &enableReporting, sizeof(enableReporting), NULL, 0, &returned, NULL, NULL) == SOCKET_ERROR)
	{
		DWORD err = WSAGetLastError();
		if (err != WSAEWOULDBLOCK)
		{
			closesocket(s);
			return NULL;
		}
	}

	__int32 tmp = 1;
	if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&tmp, sizeof(tmp)) == SOCKET_ERROR)
	{
		closesocket(s);
		return NULL;
	}
	tmp = SocketReceiveBufferBytes;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&tmp, sizeof(tmp));

	struct sockaddr_in local;
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = bindIPAddress;
    local.sin_port = htons(port);
    BOOL exclusive=TRUE;
    if(setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char*)&exclusive,sizeof(exclusive))==0 &&
        bind(s,(struct sockaddr*)&local,sizeof(local))!=SOCKET_ERROR){
        EndpointInterface* newPeer=new UdpEndpoint(s,port,this);
        peers[newPeer->endpointPort()]=newPeer;
        return newPeer;
    }
	closesocket(s);
	return NULL;
}

ChannelInterface* EndpointRegistry::makeChannel(bool control = false) { return new ReliableChannel(control); }
