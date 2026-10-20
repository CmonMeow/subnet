

bool parseServerAddress(const std::string& address, std::string& ip, unsigned short& port)
{
    const auto colon = address.find(':');
    ip = address.substr(0, colon);
    if(ip.empty() || ip.find_first_of(" \t\r\n") != std::string::npos) return false;
    if(colon == std::string::npos) return port != 0;
    unsigned value = 0;
    if(colon + 1 == address.size()) return false;
    for(size_t i = colon + 1; i < address.size(); ++i) {
        if(address[i] < '0' || address[i] > '9') return false;
        value = value * 10 + (address[i] - '0');
        if(value > 65535) return false;
    }
    if(!value) return false;
    port = static_cast<unsigned short>(value);
    return true;
}

EndpointRegistry* endpointRegistry()

{
	ensureEndpointRegistry();
	return registryStorage.GetRef();
}

void registerClientSession(ClientSession* _cl)
{
	poolCriticalSection().lock();
	_client = _cl;
	if (_cl)
	{
		endpointRegistry();
		clientEndpoint();
	}
	poolCriticalSection().unlock();
}

void registerHostSession(HostSession* _srv, unsigned short port = 777)
{
	poolCriticalSection().lock();
	_server = _srv;
	if (_srv)
	{
		endpointRegistry();
		clientEndpoint();
		hostEndpoint(true,port);
	}
	poolCriticalSection().unlock();
}

SOCKET GetServerSocket()
{
	EndpointInterface* peer = hostEndpoint();
	return peer ? peer->GetSocket() : INVALID_SOCKET;
}

ClientSessionInterface* MakeClientSession()
{
	return new ClientSession;
}

HostSessionInterface* MakeHostSession(unsigned short port)
{
	return new HostSession(port);
}

IntrusivePtr<PacketBuffer> assembleFragments(PacketBuffer* msg)
{
	if (!msg)
		return NULL;
	unsigned size = 0;
	PacketBuffer* ptr = msg;
	PacketBuffer* last;
	do
	{
		unsigned partLength = ptr->payloadLength();
		if (partLength > MAX_REASSEMBLED_USER_MESSAGE ||
			size > MAX_REASSEMBLED_USER_MESSAGE - partLength)
		{
			Error("mergeMessageList: dropping oversized user message");
			return NULL;
		}
		size += partLength;
		last = ptr;
	} while ((ptr = ptr->next.GetRef()));
	IntrusivePtr<PacketBuffer> composite = PacketCache::sharedPacketCache()->acquirePacket(size, msg->ownerChannel());
	if (!composite)
		return NULL;
	composite->assignSourceEndpoint(last);
	unsigned char* data = (unsigned char*)composite->payloadBytes();
	for (ptr = msg; ptr; ptr = ptr->next.GetRef())
	{
		memcpy(data, ptr->payloadBytes(), ptr->payloadLength());
		data += ptr->payloadLength();
	}
	composite->resizePayload(size);
	return composite;
}

bool messageListWouldExceedLimit(PacketBuffer* msg, unsigned additionalLength)
{
	if (additionalLength > MAX_REASSEMBLED_USER_MESSAGE)
		return true;
	unsigned size = additionalLength;
	for (PacketBuffer* ptr = msg; ptr; ptr = ptr->next.GetRef())
	{
		unsigned partLength = ptr->payloadLength();
		if (partLength > MAX_REASSEMBLED_USER_MESSAGE ||
			size > MAX_REASSEMBLED_USER_MESSAGE - partLength)
		{
			return true;
		}
		size += partLength;
	}
	return false;
}

PacketStatus onHostPacket(PacketBuffer* msgPtr, PacketStatus event, void* data)
{
	IntrusivePtr<PacketBuffer> msg = msgPtr;
	
	if (!_server || !msg)
		return PacketNoMoreCallbacks; 
	unsigned len = msg->payloadLength();
	if (!len)
		return PacketNoMoreCallbacks; 
	struct sockaddr_in dist;
	msg->packetDestination(dist);
	unsigned flags = msg->packetFlags();

    // Control packets on established server channels cannot remove a player.
    if (flags & PACKET_HANDSHAKE) return PacketNoMoreCallbacks;

	_server->inboundMutex.lock();
	
	if (flags & PACKET_FRAGMENT)
	{
		unsigned __int64 addrKey = udpEndpointKey(dist); 
		std::unordered_map<unsigned __int64, EndpointBinding>::iterator supportIt = _server->peerFragmentState.find(addrKey);
		EndpointBinding* sup = supportIt != _server->peerFragmentState.end() ? &supportIt->second : NULL;
		if (!sup)
		{
			_server->inboundMutex.unlock();
			return PacketNoMoreCallbacks;
		}
		bool closing = (flags & PACKET_FINAL_FRAGMENT) > 0;
		if (flags & PACKET_PRIORITY)
			if (closing) 
			{
				if (!sup->priorityFragmentsHead || !sup->priorityFragmentsTail)
				{
					_server->inboundMutex.unlock();
					return PacketNoMoreCallbacks;
				}
				sup->priorityFragmentsTail->next = msg;
				msg->next = NULL;
				msg = assembleFragments(sup->priorityFragmentsHead.GetRef());
				sup->priorityFragmentsHead = NULL;
			}
			else 
			{
				if (messageListWouldExceedLimit(sup->priorityFragmentsHead.GetRef(), msg->payloadLength()))
				{
					sup->priorityFragmentsHead = NULL;
					sup->priorityFragmentsTail = NULL;
					_server->inboundMutex.unlock();
					return PacketNoMoreCallbacks;
				}
				if (!sup->priorityFragmentsHead)
					sup->priorityFragmentsHead = msg;
				else
					sup->priorityFragmentsTail->next = msg;
				(sup->priorityFragmentsTail = msg.GetRef())->next = NULL;
				_server->inboundMutex.unlock();
				return PacketNoMoreCallbacks;
			}
		else if (closing) 
		{
			if (!sup->reliableFragmentsHead || !sup->reliableFragmentsTail)
			{
				_server->inboundMutex.unlock();
				return PacketNoMoreCallbacks;
			}
			sup->reliableFragmentsTail->next = msg;
			msg->next = NULL;
			msg = assembleFragments(sup->reliableFragmentsHead.GetRef());
			sup->reliableFragmentsHead = NULL;
		}
		else 
		{
			if (messageListWouldExceedLimit(sup->reliableFragmentsHead.GetRef(), msg->payloadLength()))
			{
				sup->reliableFragmentsHead = NULL;
				sup->reliableFragmentsTail = NULL;
				_server->inboundMutex.unlock();
				return PacketNoMoreCallbacks;
			}
			if (!sup->reliableFragmentsHead)
				sup->reliableFragmentsHead = msg;
			else
				sup->reliableFragmentsTail->next = msg;
			(sup->reliableFragmentsTail = msg.GetRef())->next = NULL;
			_server->inboundMutex.unlock();
			return PacketNoMoreCallbacks;
		}
	}

	_server->enqueueIncomingPayload(msg.GetRef());

	_server->inboundMutex.unlock();

	return PacketNoMoreCallbacks;
}

ClientSession::ClientSession()
	: receiptMutex(),
	  inboundMutex()
{
	
	lastReceiveReportedMs = GetTickCount64();
	
	channel = NULL;
	
	alreadyReceived.clear();
	split = receiveFragmentsTail = NULL;
	priorityReceiveFragments = priorityReceiveFragmentsTail = NULL;
	sent = NULL;
	sessionClosed = false;
	sessionCloseReason = DisconnectOther;
	assignedPeerId = -1; 
	registerClientSession(this);
}

ClientSession::~ClientSession()
{
	receiptMutex.lock();
	sessionClosed = true;
    sessionCloseReason = DisconnectDisconnected;

	registerClientSession(NULL);
	
	ClearIncomingPayloads();
	ClearSendReceipts();

	if (channel)
	{
		IntrusivePtr<ChannelInterface> old = channel;
		channel = NULL;
		receiptMutex.unlock();
		endpointRegistry()->removeChannel(old.GetRef());
	}
	else
		receiptMutex.unlock();
	
	TrimTransportStorage();
}

PacketStatus onClientPacket(PacketBuffer* msgPtr, PacketStatus event, void* data)
{
	IntrusivePtr<PacketBuffer> msg = msgPtr;
	
	if (!_client || !msg)
		return PacketNoMoreCallbacks; 
	_client->lastReceiveReportedMs = msg->packetTimestamp();
	unsigned len = msg->payloadLength();
	if (!len)
		return PacketNoMoreCallbacks; 

	unsigned flags = msg->packetFlags();

	bool isMagic = (flags & PACKET_HANDSHAKE) != 0;
	if (isMagic) 
	{
		if (len < 4)
			return PacketNoMoreCallbacks; 
		unsigned __int32 magic = *(unsigned __int32*)msg->payloadBytes();

		switch (magic)
		{
		case JOIN_ACCEPT_TAG: 
			if (len == sizeof(JoinReply) && _client)
			{
				JoinReply* app = (JoinReply*)msg->payloadBytes();
				_client->receiptMutex.lock();
				if (_client->joinReplyCode == JoinNone) 
				{
					_client->assignedPeerId = app->assignedPeerId;
					_client->joinReplyCode = app->result;
				}
				_client->receiptMutex.unlock();
			}
			break; 


		}

		return PacketNoMoreCallbacks;
	}

	_client->inboundMutex.lock();
	
	if (flags & PACKET_FRAGMENT)
	{
		bool closing = (flags & PACKET_FINAL_FRAGMENT) > 0;
		if (flags & PACKET_PRIORITY)
			if (closing) 
			{
				if (!_client->priorityReceiveFragments || !_client->priorityReceiveFragmentsTail)
				{
					_client->inboundMutex.unlock();
					return PacketNoMoreCallbacks;
				}
				_client->priorityReceiveFragmentsTail->next = msg;
				msg->next = NULL;
				msg = assembleFragments(_client->priorityReceiveFragments.GetRef());
				_client->priorityReceiveFragments = NULL;
			}
			else 
			{
				if (messageListWouldExceedLimit(_client->priorityReceiveFragments.GetRef(), msg->payloadLength()))
				{
					_client->priorityReceiveFragments = NULL;
					_client->priorityReceiveFragmentsTail = NULL;
					_client->inboundMutex.unlock();
					return PacketNoMoreCallbacks;
				}
				if (!_client->priorityReceiveFragments)
					_client->priorityReceiveFragments = msg;
				else
					_client->priorityReceiveFragmentsTail->next = msg;
				(_client->priorityReceiveFragmentsTail = msg.GetRef())->next = NULL;
				_client->inboundMutex.unlock();
				return PacketNoMoreCallbacks;
			}
		else if (closing) 
		{
			if (!_client->split || !_client->receiveFragmentsTail)
			{
				_client->inboundMutex.unlock();
				return PacketNoMoreCallbacks;
			}
			_client->receiveFragmentsTail->next = msg;
			msg->next = NULL;
			msg = assembleFragments(_client->split.GetRef());
			_client->split = NULL;
		}
		else 
		{
			if (messageListWouldExceedLimit(_client->split.GetRef(), msg->payloadLength()))
			{
				_client->split = NULL;
				_client->receiveFragmentsTail = NULL;
				_client->inboundMutex.unlock();
				return PacketNoMoreCallbacks;
			}
			if (!_client->split)
				_client->split = msg;
			else
				_client->receiveFragmentsTail->next = msg;
			(_client->receiveFragmentsTail = msg.GetRef())->next = NULL;
			_client->inboundMutex.unlock();
			return PacketNoMoreCallbacks;
		}
	}

	if (msg)
		_client->enqueueIncomingPayload(msg.GetRef());

	_client->inboundMutex.unlock();

	return PacketNoMoreCallbacks;
}

PacketStatus onClientSendFinished(PacketBuffer* msg, PacketStatus event, void* data)
{
	ClientSession* client = (ClientSession*)data;
	if (!client || !msg || client->sessionClosed)
		return PacketNoMoreCallbacks; 

	client->receiptMutex.lock();
	msg->next = client->sent;
	client->sent = msg;
	client->receiptMutex.unlock();
	return PacketNoMoreCallbacks;
}

IntrusivePtr<PacketBuffer> ClientSession::SendPayload(BYTE* buffer, __int32 bufferSize, DWORD& msgID, DeliveryOptions flags, const IntrusivePtr<PacketBuffer>& dependOn)
{
	bool vim = (flags & DeliveryGuaranteed) > 0;
	bool urgent = (flags & DeliveryHighPriority) > 0;
	receiptMutex.lock();
	if (!channel || !buffer || bufferSize <= 0)
	{
		receiptMutex.unlock();
		return NULL;
	}
	__int32 maxMessage = DATAGRAM_PAYLOAD_LIMIT_BYTES;
	if (!vim && bufferSize > maxMessage)
	{
		receiptMutex.unlock();

		Error("ClientSession: trying to send too large non-guaranteed message (%d bytes long)", bufferSize);
		return NULL;
	}
	IntrusivePtr<PacketBuffer> msg;
	if (vim)
	{ 
		unsigned fl = PACKET_RELIABLE | (urgent ? PACKET_PRIORITY : 0);
		if (bufferSize > maxMessage)
		{ 

			__int32 toSent = bufferSize;
			__int32 packet;
			fl |= PACKET_FRAGMENT;
			do
			{
				packet = (toSent > maxMessage) ? maxMessage : toSent;
				toSent -= packet;
				msg = PacketCache::sharedPacketCache()->acquirePacket(packet, channel.GetRef());
				if (!msg)
				{
					receiptMutex.unlock();
					return NULL;
				}
				msg->updatePacketFlags(PACKET_FLAG_MASK, fl | (toSent ? 0 : PACKET_FINAL_FRAGMENT));
				msg->setOrderingDependency();
				msg->assignPayload((unsigned char*)buffer, packet);
				buffer += packet;
				msg->queueForSend(urgent);
			} while (toSent);
		}
		else
		{ 
			msg = PacketCache::sharedPacketCache()->acquirePacket(bufferSize, channel.GetRef());
			msg->updatePacketFlags(PACKET_FLAG_MASK, fl);
			msg->setOrderingDependency();
			msg->assignPayload((unsigned char*)buffer, bufferSize);
			msg->queueForSend(urgent);
		}
	}
	else
	{ 
		msg = PacketCache::sharedPacketCache()->acquirePacket(bufferSize, channel.GetRef());
		if (!msg)
		{
			receiptMutex.unlock();
			return NULL;
		}
		if (dependOn)
			msg->requireOrderedDelivery(dependOn.GetRef());
		if (flags & DeliverySetCallback)
			msg->setPacketCallback(onClientSendFinished, PacketOutputSent, this);
		msg->setDeliveryDeadline(BestEffortLifetimeMs);
		msg->assignPayload((unsigned char*)buffer, bufferSize);
		msg->queueForSend();
	}
	msgID = (DWORD)msg->id;

	receiptMutex.unlock();
	return msg;
}

void ClientSession::QueryPendingSends(__int32& nMsg, __int32& nBytes, __int32& nMsgG, __int32& nBytesG)
{
	receiptMutex.lock();
	if (channel)
		channel->queryPacketBacklog(nMsg, nBytes, nMsgG, nBytesG);
	else
	{
		nMsg = nBytes = nMsgG = nBytesG = 0;
	}
	receiptMutex.unlock();
}

void ClientSession::GetConnectionLimits(__int32& maxBandwidthPerClient)
{
	maxBandwidthPerClient = INT_MAX;
}

bool ClientSession::QueryConnectionMetrics(__int32& latencyMS, __int32& throughputBPS)
{
	receiptMutex.lock();
	if (!channel)
	{
		receiptMutex.unlock();
		return false;
	}
	latencyMS = (__int32)channel->roundTripTime();
	throughputBPS = (__int32)channel->estimatedSendRate();

	receiptMutex.unlock();
	return true;
}

bool ClientSession::GetLocalAddress(in_addr& addr) const
{
	struct sockaddr_in local;
	if (!localEndpointAddress(local, 0))
		return false;
	addr = local.sin_addr;
	return true;
}

bool ClientSession::GetLocalAddress(in_addr& addr, __int32& port) const
{
	receiptMutex.lock();
	if (!channel)
	{
		receiptMutex.unlock();
		return false;
	}
	struct sockaddr_in saddr;
	channel->localEndpointAddress(saddr);
	addr = saddr.sin_addr;
	port = ntohs(saddr.sin_port);
	receiptMutex.unlock();
	return true;
}

bool ClientSession::GetDistantAddress(in_addr& addr, __int32& port) const
{
	receiptMutex.lock();
	if (!channel)
	{
		receiptMutex.unlock();
		return false;
	}
	struct sockaddr_in saddr;
	channel->remoteEndpointAddress(saddr);
	addr = saddr.sin_addr;
	port = saddr.sin_port;
	receiptMutex.unlock();
	return true;
}

bool ClientSession::GetServerAddress(sockaddr_in& address) const
{
	receiptMutex.lock();
	if (!channel)
	{
		receiptMutex.unlock();
		return false;
	}
	channel->remoteEndpointAddress(address);
	receiptMutex.unlock();
	return true;
}

bool ClientSession::HasDisconnected()
{
	receiptMutex.lock();
	if (!channel)
	{
		receiptMutex.unlock();
		return true;
	}
	
	if (!localBotRole && channel->hasTimedOut())
	{
		sessionClosed = true;
		sessionCloseReason = DisconnectTimeout;
	}
    const bool terminated = sessionClosed;
    receiptMutex.unlock();
    return terminated;
}

DisconnectReason ClientSession::DisconnectCause()
{
	DisconnectReason reason;
	receiptMutex.lock();
	reason = sessionCloseReason;

	receiptMutex.unlock();
	return reason;
}

std::string ClientSession::GetWhySessionTerminatedStr()
{
	std::string reason;
	receiptMutex.lock();
	reason = whySessionTerminatedStr;

	receiptMutex.unlock();
	return reason;
}

void ClientSession::DrainIncomingPayloads(ClientPayloadHandler* callback, void* context)
{
	if (!callback)
		return;
	LARGE_INTEGER started, frequency;
	QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
	IntrusivePtr<PacketBuffer> msg;
	inboundMutex.lock();
	auto overloaded = alreadyReceived.takeOverloaded();
	inboundMutex.unlock();
	if (!overloaded.empty())
	{
		std::lock_guard<std::recursive_mutex> lock(receiptMutex);
		sessionClosed = true;
		sessionCloseReason = DisconnectOther;
		strcpy_s(whySessionTerminatedStr, "receive queue exceeded");
		ClearIncomingPayloads();
		return;
	}
	inboundMutex.lock();

	for (unsigned count = 0; count < 256 && !alreadyReceived.empty(); ++count)
	{
		msg = alreadyReceived.pop();
		if (!(msg->packetFlags() & PACKET_RELIABLE) && GetTickCount64() - msg->packetTimestamp() > BestEffortLifetimeMs) continue;
		inboundMutex.unlock();

		(*callback)((char*)msg->payloadBytes(), msg->payloadLength(), context);
		inboundMutex.lock();
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		if ((now.QuadPart - started.QuadPart) * 1000 >= frequency.QuadPart * 8) break;
	}
	inboundMutex.unlock();
}

void ClientSession::enqueueIncomingPayload(PacketBuffer* msg)
{
	alreadyReceived.push(msg, false);
}

void ClientSession::ClearIncomingPayloads()
{
	std::lock_guard<std::recursive_mutex> lock(inboundMutex);
	alreadyReceived.clear();
}

void ClientSession::ClearSendReceipts()
{
	receiptMutex.lock();
	IntrusivePtr<PacketBuffer> tmp;
	while (sent)
	{
		tmp = sent->next;
		sent->next = NULL; 
		sent = tmp;
	}
	receiptMutex.unlock();
}

unsigned ClientSession::TrimTransportStorage()
{
	if (!PacketCache::sharedPacketCache())
		return 0;
	unsigned ret = PacketCache::sharedPacketCache()->trimCachedPackets();
	return ret;
}

HostSession::HostSession(unsigned short port)
	: peerStateMutex(),
	  inboundMutex(),
	  receiptMutex()
{
	
	peerStateMutex.lock();
	alreadyReceived.clear();
	sent = NULL;
	acceptConnections = true;
	session.serverState = 0;
	session.roomCapacity = 0xff;
	session.connectedPeerCount = 0;
	session.password = false;
	session.port = 0;
	session.name[0] = (char)0;

	registerHostSession(this,port);
	poolCriticalSection().lock();
	roomListenPort = hostEndpoint(true,port) ? hostEndpoint()->endpointPort() : 0;
	poolCriticalSection().unlock();
	peerStateMutex.unlock();
}

HostSession::~HostSession()
{
	DiscardOutbound();
	registerHostSession(NULL);
    // Release registered channels as well as the user map. Otherwise a later
    // host in this process finds orphaned endpoints and rejects reconnects.
    {
        std::lock_guard<std::recursive_mutex> poolGuard(poolCriticalSection());
        std::lock_guard<std::recursive_mutex> userGuard(peerStateMutex);
        PeerChannelTable::Iterator iterator;
        __int32 player;
        IntrusivePtr<ChannelInterface> old;
        while (users.getFirst(iterator, old, &player))
        {
            users.removeKey(player);
            endpointRegistry()->removeChannel(old.GetRef());
        }
    }
	
	ClearIncomingPayloads();
	ClearSendReceipts();
	
	ClearPeerEvents();
	
	TrimTransportStorage();
}

void HostSession::GetTrafficTotals(unsigned __int64& incoming, unsigned __int64& outgoing) const
{
    std::lock_guard<std::recursive_mutex> guard(poolCriticalSection());
    EndpointInterface* peer = hostEndpoint(false);
    incoming = outgoing = 0;
    if (peer) peer->getTrafficTotals(incoming, outgoing);
}

void HostSession::QueryPendingSends(__int32 to, __int32& nMsg, __int32& nBytes, __int32& nMsgG, __int32& nBytesG)
{
	peerStateMutex.lock();
	IntrusivePtr<ChannelInterface> channel;
	if (users.get(to, channel))
		channel->queryPacketBacklog(nMsg, nBytes, nMsgG, nBytesG);
	else
	{
		nMsg = nBytes = nMsgG = nBytesG = 0;
	}
	peerStateMutex.unlock();
}

bool HostSession::QueryConnectionMetrics(__int32 to, __int32& latencyMS, __int32& throughputBPS)
{
	peerStateMutex.lock();
	IntrusivePtr<ChannelInterface> channel;
	if (!users.get(to, channel))
	{
		peerStateMutex.unlock();
		return false;
	}
	latencyMS = (__int32)channel->roundTripTime();
	throughputBPS = (__int32)channel->estimatedSendRate();

	peerStateMutex.unlock();
	return true;
}

void HostSession::GetConnectionLimits(__int32& maxBandwidthPerClient)
{
	maxBandwidthPerClient = transportTuning.maximumSendRateBytesPerSecond;
}

void HostSession::disconnectRemotePeer(ChannelInterface* ch, DisconnectReason, const char*)
{
    // Application code sends the encrypted notice before requesting local cleanup.
    if (ch) finalizePeerRemoval(findPeerIdForChannel(ch));
}

void HostSession::finalizePeerRemoval(__int32 player)
{
	if (player == -1)
	{
		Error("HostSession::finishDestroyPlayer(%d): invalid player", player);
		return;
	}

	peerStateMutex.lock();
	IntrusivePtr<ChannelInterface> ch;
	if (!users.get(player, ch))
	{
		Error("HostSession::finishDestroyPlayer(%d): users.get failed", player);
		peerStateMutex.unlock();
		return;
	}
	
	users.removeKey(player);
	struct sockaddr_in daddr;
	ch->remoteEndpointAddress(daddr);
	{ std::lock_guard<std::recursive_mutex> lock(inboundMutex); peerFragmentState.erase(udpEndpointKey(daddr)); }
	endpointRegistry()->removeChannel(ch.GetRef());
	for (std::vector<PeerJoinedEvent>::iterator it = pendingJoins.begin(); it != pendingJoins.end(); ++it)
		if (it->player == player)
		{
			pendingJoins.erase(it);
			peerStateMutex.unlock();
			return;
		}
	pendingDepartures.push_back(PeerLeftEvent());
	PeerLeftEvent& info = pendingDepartures.back();
	info.player = player;
	peerStateMutex.unlock();
}

void HostSession::DisconnectPeer(__int32 player, DisconnectReason reason, const char* reasonStr)
{
	peerStateMutex.lock();
	IntrusivePtr<ChannelInterface> channel;
	if (!users.get(player, channel))
	{
		Error("HostSession::DisconnectPeer: player=%d - !users.get", player);
		peerStateMutex.unlock();
		return;
	}

	disconnectRemotePeer(channel.GetRef(), reason, reasonStr);
	peerStateMutex.unlock();
}

bool HostSession::StartSession(std::string name, std::string password, __int32 port)
{
	if (!roomListenPort || roomListenPort != port)
	{
		Error("Cannot start host on port %d.", port);
		return false;
	}
	
	peerStateMutex.lock();
	_password = password;
	session.roomCapacity = 0xff;
	session.connectedPeerCount = 0;
	session.password = (password.length() > 0) * 2;
	session.port = roomListenPort;
	strncpy(session.name, name.c_str(), sizeof(session.name));
	session.name[sizeof(session.name) - 1] = (char)0;
	roomDisplayName = name;
	session.serverState = 0;

	peerStateMutex.unlock();
	return true;
}

void HostSession::UpdateLockedOrPassworded(bool lock, bool passworded)
{
	session.password = (lock ? 1 : 0) + (passworded ? 2 : 0);
}

PacketStatus onHostSendFinished(PacketBuffer* msg, PacketStatus event, void* data)

{
	HostSession* server = (HostSession*)data;
	if (!server || !msg)
		return PacketNoMoreCallbacks; 

	server->receiptMutex.lock();
	msg->next = server->sent;
	server->sent = msg;
	server->receiptMutex.unlock();
	return PacketNoMoreCallbacks;
}

IntrusivePtr<PacketBuffer> HostSession::SendPayload(__int32 to, BYTE* buffer, __int32 bufferSize, DWORD& msgID, DeliveryOptions flags, const IntrusivePtr<PacketBuffer>& dependOn)
{
	if (!buffer || bufferSize <= 0)
	{
		Error("HostSession: trying to send empty message to %d", to);
		return NULL;
	}
	IntrusivePtr<ChannelInterface> channel;
	IntrusivePtr<PacketBuffer> msg;
	bool vim = (flags & DeliveryGuaranteed) > 0;
	bool urgent = (flags & DeliveryHighPriority) > 0;
	bool setPacketCallback = (flags & DeliverySetCallback) > 0;
	__int32 maxMessage = DATAGRAM_PAYLOAD_LIMIT_BYTES;
	if (!hostEndpoint())
	{
		Error("HostSession: unable to get peer.");
		return NULL;
	}
	if ((!vim || to == BROADCAST_PEER_ID) && bufferSize > maxMessage)
	{

		Error("HostSession: trying to send too large non-guaranteed message (%d bytes long, max %d allowed)", bufferSize, maxMessage);
		return NULL;
	}
	unsigned short fl = 0;
	if (vim)
		fl |= PACKET_RELIABLE;
	if (urgent)
		fl |= PACKET_PRIORITY;

	peerStateMutex.lock();
	if (to == BROADCAST_PEER_ID)
	{ 

		PeerChannelTable::Iterator it;
		if (users.getFirst(it, channel))
			do
			{
				msg = PacketCache::sharedPacketCache()->acquirePacket(bufferSize, channel.GetRef());
				if (!msg)
				{
					peerStateMutex.unlock();
					Error("HostSession: pool()->newMessage failed when sending to %d", to);
					return NULL;
				}
				msg->updatePacketFlags(PACKET_FLAG_MASK, fl);
				if (fl)
				{
					if (setPacketCallback)
						msg->setPacketCallback(onHostSendFinished, PacketOutputSent, this);
					msg->setOrderingDependency(); 
				}
				else 
				{
					if (setPacketCallback)
						msg->setPacketCallback(onHostSendFinished, PacketOutputSent, this);
					if (dependOn)
						msg->requireOrderedDelivery(dependOn.GetRef());
					msg->setDeliveryDeadline(BestEffortLifetimeMs);
				}
				msg->assignPayload((unsigned char*)buffer, bufferSize);
				msg->queueForSend(urgent);

			} while (users.getNext(it, channel));
	}

	else 
	{
		if (!users.get(to, channel))
		{
			// Disconnect can remove the channel before the application consumes
			// its departure event. Reject in-flight voice/data without logging
			// this normal race; checking at the caller cannot make it atomic.
			peerStateMutex.unlock();
			return NULL;
		}
		if (bufferSize > maxMessage) 
		{

			__int32 toSent = bufferSize;
			__int32 packet;
			fl |= PACKET_FRAGMENT;
			do
			{
				packet = (toSent > maxMessage) ? maxMessage : toSent;
				toSent -= packet;
				msg = PacketCache::sharedPacketCache()->acquirePacket(packet, channel.GetRef());
				if (!msg)
				{
					peerStateMutex.unlock();
					Error("HostSession: pool()->newMessage failed when sending to %d", to);
					return NULL;
				}
				msg->updatePacketFlags(PACKET_FLAG_MASK, fl | (toSent ? 0 : PACKET_FINAL_FRAGMENT));
				if (setPacketCallback && !toSent) 
					msg->setPacketCallback(onHostSendFinished, PacketOutputSent, this);
				msg->setOrderingDependency();
				msg->assignPayload((unsigned char*)buffer, packet);
				buffer += packet;
				msg->queueForSend(urgent);
			} while (toSent);
		}
		else 
		{
			msg = PacketCache::sharedPacketCache()->acquirePacket(bufferSize, channel.GetRef());
			if (!msg)
			{
				peerStateMutex.unlock();
				Error("HostSession: pool()->newMessage failed when sending to %d", to);
				return NULL;
			}
			msg->updatePacketFlags(PACKET_FLAG_MASK, fl);
			if (fl)
			{
				if (setPacketCallback)
					msg->setPacketCallback(onHostSendFinished, PacketOutputSent, this);
				msg->setOrderingDependency(); 
			}
			else 
			{
				if (setPacketCallback)
					msg->setPacketCallback(onHostSendFinished, PacketOutputSent, this);
				if (dependOn)
					msg->requireOrderedDelivery(dependOn.GetRef());
				msg->setDeliveryDeadline(BestEffortLifetimeMs);
			}
			msg->assignPayload((unsigned char*)buffer, bufferSize);
			msg->queueForSend(urgent);
		}
	}

	msgID = (DWORD)msg->id;
	peerStateMutex.unlock();
	return msg;
}

void HostSession::DiscardOutbound()
{
	poolCriticalSection().lock();
	if (hostEndpoint(false))
		hostEndpoint(false)->discardPendingPackets();
	poolCriticalSection().unlock();
}

void HostSession::SetRoomState(__int32 state)
{
	peerStateMutex.lock();
	session.serverState = state;
	peerStateMutex.unlock();
}

bool HostSession::FormatListenAddress(char* address, DWORD addressLen)
{
	if (!address || !addressLen) return false;
	address[0] = 0;
	std::lock_guard<std::recursive_mutex> lock(poolCriticalSection());
	sockaddr_in local = {};
	if (!hostEndpoint() || !localEndpointAddress(local, roomListenPort)) return false;
	const int length = snprintf(address, addressLen, "%u.%u.%u.%u:%u",
		(unsigned)IPV4_OCTET_1(local), (unsigned)IPV4_OCTET_2(local),
		(unsigned)IPV4_OCTET_3(local), (unsigned)IPV4_OCTET_4(local), (unsigned)roomListenPort);
	if (length < 0 || (DWORD)length >= addressLen)
	{
		address[0] = 0;
		return false;
	}
	return true;
}

bool HostSession::GetServerAddress(sockaddr_in& address)
{
	return localEndpointAddress(address, roomListenPort);
}

bool HostSession::GetClientAddress(__int32 client, sockaddr_in& address)
{
	auto channel = playerToChannel(client);
	if (!channel) return false;
	channel->remoteEndpointAddress(address);
	return true;
}

__int32 HostSession::findPeerIdForChannel(ChannelInterface* ch)
{
	std::lock_guard<std::recursive_mutex> lock(peerStateMutex);
	return users.peerId(ch);
}

void HostSession::DrainIncomingPayloads(HostPayloadHandler* callback, void* context)
{
	if (!callback)
		return;
	LARGE_INTEGER started, frequency;
	QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
	IntrusivePtr<PacketBuffer> msg;
	inboundMutex.lock();
	auto overloaded = alreadyReceived.takeOverloaded();
	inboundMutex.unlock();
	for (const auto& channel : overloaded)
	{
		const int player = findPeerIdForChannel(channel.GetRef());
		if (player >= RESERVED_PEER_ID_COUNT)
		{
			Error("Player %d exceeded the receive queue limit", player);
			finalizePeerRemoval(player);
		}
	}
	inboundMutex.lock();

	for (unsigned count = 0; count < 256 && !alreadyReceived.empty(); ++count)
	{
		msg = alreadyReceived.pop();
		if (!(msg->packetFlags() & PACKET_RELIABLE) && GetTickCount64() - msg->packetTimestamp() > BestEffortLifetimeMs) continue;
		
		inboundMutex.unlock();
		__int32 player = findPeerIdForChannel(msg->ownerChannel());
		if (player >= RESERVED_PEER_ID_COUNT)
			(*callback)(player, (char*)msg->payloadBytes(), msg->payloadLength(), context);
		inboundMutex.lock();
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		if ((now.QuadPart - started.QuadPart) * 1000 >= frequency.QuadPart * 8) break;
	}
	inboundMutex.unlock();
}

void HostSession::enqueueIncomingPayload(PacketBuffer* msg)
{
	alreadyReceived.push(msg, true);
}

void HostSession::ClearIncomingPayloads()
{
	std::lock_guard<std::recursive_mutex> lock(inboundMutex);
	alreadyReceived.clear();
}

void HostSession::ClearSendReceipts()
{
	receiptMutex.lock();
	IntrusivePtr<PacketBuffer> tmp;
	while (sent)
	{
		tmp = sent->next;
		sent->next = NULL; 
		sent = tmp;
	}
	receiptMutex.unlock();
}

void HostSession::DrainPeerEvents(PeerJoinedHandler* callbackCreate, PeerLeftHandler* callbackDelete, void* context)
{
	peerStateMutex.lock();
	// Timeout enforcement is independent of display-statistics sampling.
	for (auto peer : users.timedOutPeers()) finalizePeerRemoval(peer);
	for (size_t i = 0; i < pendingDepartures.size(); i++)
	{
		PeerLeftEvent& info = pendingDepartures[i];
		
		callbackDelete(info.player, context);

		session.connectedPeerCount--;
	}
	if (session.connectedPeerCount < 0)
		session.connectedPeerCount = 0;
	pendingDepartures.clear();
	for (size_t i = 0; i < pendingJoins.size(); i++)
	{
		PeerJoinedEvent& info = pendingJoins[i];
		
		callbackCreate(info.player, info.reservedBotRole, info.name, info.inaddr, context);
		session.connectedPeerCount++;
	}
	pendingJoins.clear();
	peerStateMutex.unlock();
}

void HostSession::ClearPeerEvents()
{
	peerStateMutex.lock();
	pendingJoins.clear();
	pendingDepartures.clear();
	peerStateMutex.unlock();
}

unsigned HostSession::TrimTransportStorage()
{
	if (!PacketCache::sharedPacketCache())
		return 0;
	unsigned ret = PacketCache::sharedPacketCache()->trimCachedPackets();
	return ret;
}

IntrusivePtr<ChannelInterface> HostSession::playerToChannel(__int32 player)
{
	IntrusivePtr<ChannelInterface> channel;
	peerStateMutex.lock();
	users.get(player, channel);
	peerStateMutex.unlock();
	return channel;
}
