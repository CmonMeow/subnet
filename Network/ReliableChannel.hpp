#ifdef _MSC_VER
#pragma once
#endif

#ifndef NETCHANNEL_H
#define NETCHANNEL_H

#include "UdpEndpoint.hpp"
#include <Network/SequenceBitmap.hpp>

extern unsigned __int64 packetSequenceKey(const IntrusivePtr<PacketBuffer>& msg);
extern unsigned __int64 packetDependencyKey(const IntrusivePtr<PacketBuffer>& msg);

#define ACK_RING_CAPACITY 1024 

#define RECENT_ACK_CAPACITY 8

#define RTT_SMOOTHING_WEIGHT 0.30f 

#define BANDWIDTH_SMOOTHING_WEIGHT 0.15f		
#define LOCAL_BANDWIDTH_SMOOTHING_WEIGHT 0.25f 

#define RECEIVER_BANDWIDTH_ESTIMATE_WEIGHT 0.00f 

#define PING_REPLY_TIMEOUT_MS 6000
#define PING_RETRY_INTERVAL_MS 3000
#define HEARTBEAT_INTERVAL_LIMIT_MS 2000

#define RETRANSMIT_DELAY_LIMIT_MS 7000

#define BASE_RETRANSMIT_DELAY_LIMIT_MS 4000

#define ACK_SLOT_EMPTY 0xff 

#define PACKET_PAIR_SEND_GAP_MS 50

#define PACKET_PAIR_TARGET_SECONDS 0.025f 

#define PACKET_PAIR_MIN_BYTES 50 

#define PACKET_PAIR_MAX_BYTES 800 

#define PACKET_PAIR_DELAY_LIMIT_MS 200

#define RECEIVE_WINDOW_TRIM_SLACK 4096 

// Local bookkeeping only. Wire sequences and the datagram header remain 32-bit.
constexpr unsigned __int64 MAX_LOCAL_SEQUENCE = 0x7fffffff00000000ull;

// Select the nearest 32-bit wrap relative to a known local sequence. Peers must
// remain within half the wire sequence space; receive windows are far smaller.
inline bool expandWireSequence(unsigned __int32 wire, unsigned __int64 reference, unsigned __int64& result)
{
    const unsigned __int32 forward = wire - static_cast<unsigned __int32>(reference);
    if (forward == 0x80000000u) return false;
    if (forward < 0x80000000u)
    {
        result = reference + forward;
        return result >= reference && result < MAX_LOCAL_SEQUENCE;
    }
    const unsigned __int32 backward = 0u - forward;
    if (reference < backward) return false;
    result = reference - backward;
    return true;
}

#define ACK_RATE_SAMPLE_INTERVAL_MS 100

#define PRIORITY_QUEUE_THRESHOLD 65 

#define RELIABLE_QUEUE_THRESHOLD 25 

class ReliableChannel : public ChannelInterface
{
protected:
	
	EndpointInterface* peer;
	unsigned timeout;
	bool control;

public:
	ReliableChannel(bool _control)
	{
		starvation = false;
		opened = false;
		timeout = 1000;
		control = _control;
		memset((void*)&(dist), NULL, sizeof(dist));
		dist.sin_addr.s_addr = INADDR_BROADCAST;
		peer = NULL;
		
		highestReceivedSequence = ackRingFirstSequence = receivedSerialWindowStart = serial = 1;
		ackRingCursor = 1;
		lastSentSequence = -1;
		recentAckHead = recentAckTail = 0;
		smoothedRttMs = latestRttMs = 0;
		heartbeatIntervalMs = timeout;
		
		priorityQueueHead = NULL;
		reliableQueueHead = NULL;
		bestEffortQueueHead = NULL;
		
		lastPingReceivedMs = lastPingSentMs =
			lastPacketReceivedMs = lastPacketSentMs =
				lastProbePairSentMs = lastRetransmitScanMs = GetTickCount64();
		reliablePacketsSinceAck = 0;
		ackMin = ackMax = 0;
		
		memset((void*)&(ackRateSamples), NULL, sizeof(ackRateSamples));
		for (__int32 i = 2; i < AckRateSampleSlots; i += 2)
			ackRateSamples[i] = lastRetransmitScanMs - transportTuning.ackRateWindowMs;
		ackRateSamples[0] = lastRetransmitScanMs;
		ackRateSamples[1] = (transportTuning.initialSendRateBytesPerSecond * (unsigned __int64)transportTuning.ackRateWindowMs) / 1000;
		ackRateWriteIndex = 0;
		
		resetSendRateSamples(lastRetransmitScanMs);

		memset((void*)&(ackTime), NULL, sizeof(ackTime));
	}

	virtual PacketStatus openChannel(EndpointInterface* _peer, struct sockaddr_in& distant)
	{
		if (opened || !_peer)
			return PacketError;
		peer = _peer;
		dist = distant;
		if (!control && !peer->attachChannel(distant, this))
			return PacketInvalidSharing;

		stateMutex.lock();

		opened = true;
		highestReceivedSequence = ackRingFirstSequence = receivedSerialWindowStart = serial = 1;
		ackRingCursor = 1;
		lastSentSequence = -1;
		recentAckHead = recentAckTail = 0;
		for (__int32 i = 0; i < ACK_RING_CAPACITY;)
			ack[i++] = ACK_SLOT_EMPTY;
		smoothedRttMs = latestRttMs = 0;
		lastPacketReceivedMs = lastRetransmitScanMs = GetTickCount64();
		lastPingReceivedMs = lastRetransmitScanMs - PING_REPLY_TIMEOUT_MS;
		lastPingSentMs = lastRetransmitScanMs - PING_RETRY_INTERVAL_MS;
		lastProbePairSentMs = lastRetransmitScanMs - HEARTBEAT_INTERVAL_LIMIT_MS;
		lastPacketSentMs = lastRetransmitScanMs - heartbeatIntervalMs;
		reliablePacketsSinceAck = 0;
		
		ackMin = ackMax = 0;
		
		memset((void*)&(ackRateSamples), NULL, sizeof(ackRateSamples));
		for (__int32 i = 2; i < AckRateSampleSlots; i += 2)
			ackRateSamples[i] = lastRetransmitScanMs - transportTuning.ackRateWindowMs;
		ackRateSamples[0] = lastRetransmitScanMs;
		ackRateSamples[1] = (transportTuning.initialSendRateBytesPerSecond * (unsigned __int64)transportTuning.ackRateWindowMs) / 1000;
		ackRateWriteIndex = 0;
		
		resetSendRateSamples(lastRetransmitScanMs);

		stateMutex.unlock();
		return PacketOK;
	}

	virtual bool isHandshakeChannel() const { return control; }

	virtual unsigned roundTripTime()
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		return smoothedRttMs;
	}

	virtual unsigned estimatedSendRate()
	{
		stateMutex.lock();
		float ackBandwidth = estimateAcknowledgedRate(transportTuning.ackRateWindowMs);
		stateMutex.unlock();

		return ackBandwidth > 0.0f ? static_cast<unsigned>(ackBandwidth) : 0;
	}

	virtual unsigned __int64 lastReceiveTime() const
	{
		stateMutex.lock();
		unsigned __int64 result = lastPacketReceivedMs;
		stateMutex.unlock();
		return result;
	}

	virtual void queryPacketBacklog(__int32& msgs, __int32& bytes, __int32& vimMsgs, __int32& vimBytes)
	{
		msgs = bytes = vimMsgs = vimBytes = 0;
		stateMutex.lock();
		PacketBuffer* tmp = reliableQueueHead.GetRef();
		while (tmp)
		{
			vimMsgs++; 
			vimBytes += tmp->header->length + IPV4_UDP_HEADER_BYTES;
			tmp = tmp->next.GetRef();
		}
		tmp = priorityQueueHead.GetRef();
		while (tmp)
		{
			vimMsgs++;
			vimBytes += tmp->header->length + IPV4_UDP_HEADER_BYTES;
			tmp = tmp->next.GetRef();
		}
		tmp = bestEffortQueueHead.GetRef();
		while (tmp)
		{
			msgs++;
			bytes += tmp->header->length + IPV4_UDP_HEADER_BYTES;
			tmp = tmp->next.GetRef();
		}
		stateMutex.unlock();
	}

	virtual void remoteEndpointAddress(struct sockaddr_in& distant) const { distant = dist; }
	virtual void localEndpointAddress(struct sockaddr_in& local) const { peer ? peer->localEndpointAddress(local) : memset((void*)&(local), 0, sizeof(local)); }

	virtual void sendRaw(const sockaddr_in& ia, const void* data, __int32 size, __int32 sizeEncrypted) { peer->sendRaw(ia, data, size, sizeEncrypted); }

	virtual void handleDatagram(DatagramHeader* hdr, const struct sockaddr_in& distant)
	{
		stateMutex.lock();
		// These fields are not authenticated by the application. Bound the work
		// they can request before allocating a message or touching the bit masks.
        unsigned __int64 incomingSequence = hdr->serial;
        unsigned __int64 predecessorSequence = 0;
        if (!control && !(hdr->flags & PACKET_FROM_CONTROL_CHANNEL))
        {
            if (!hdr->serial || !expandWireSequence(hdr->serial, highestReceivedSequence, incomingSequence) ||
                (incomingSequence > highestReceivedSequence && incomingSequence - highestReceivedSequence > transportTuning.receiveSequenceWindowSize) ||
                ((hdr->flags & (PACKET_RELIABLE | PACKET_ORDERED)) == (PACKET_RELIABLE | PACKET_ORDERED) && hdr->ordered.predecessorSequence &&
                 (!expandWireSequence(hdr->ordered.predecessorSequence, incomingSequence, predecessorSequence) || predecessorSequence >= incomingSequence)))
            {
                stateMutex.unlock();
                return;
            }
        }
		IntrusivePtr<PacketBuffer> msg = PacketCache::sharedPacketCache()->acquirePacket(hdr->length - sizeof(DatagramHeader), this);
		if (!msg)
		{
			stateMutex.unlock();
			return;
		}
		if (!msg->copyDatagram(hdr))
		{
			msg->releaseToCache();
			stateMutex.unlock();
			return;
		}

		msg->localSequence = incomingSequence;

		if (!control && alreadyReceived(incomingSequence))
		{
			recordRetransmission(msg.GetRef());
			msg->releaseToCache();
			stateMutex.unlock();
			return;
		}
		const unsigned short flags = hdr->flags;
		
		msg->packetEventHandler = receiveCallback; 
		msg->packetEventContext = data;
		for (__int32 i = 0; i < subsets.size(); i++)
			if ((flags & subsets[i].andFlag) == subsets[i].eqFlag)
			{
				msg->packetEventHandler = subsets[i].receiveCallback;
				msg->packetEventContext = subsets[i].data;
				break;
			}
		msg->distant = distant;
		msg->subscribedPacketEvent = msg->status = PacketInputReceived;

		if (!control && !(flags & PACKET_FROM_CONTROL_CHANNEL))
		{ 
			const unsigned __int64 orderingPredecessor = predecessorSequence;
			if ((flags & (PACKET_RELIABLE | PACKET_ORDERED)) == (PACKET_RELIABLE | PACKET_ORDERED) &&
				orderingPredecessor >= receivedSerialWindowStart && !processedSerials.get(orderingPredecessor) &&
				(deferredMessages >= 1024 || deferredBytes + msg->payloadLength() > 1024 * 1024))
			{
				// Do not acknowledge data we cannot retain. A legitimate sender
				// can retry after the missing predecessor arrives.
				stateMutex.unlock();
				return;
			}
			updateReceiveMetrics(msg.GetRef());
			
			if (flags & PACKET_PING_REQUEST)
				queueDelayedAcknowledgement(msg.GetRef());

			if (flags & PACKET_HEADER_ONLY)
			{
				stateMutex.unlock();
				return;
			}
			
			if (flags & PACKET_RELIABLE)
			{
				if (flags & PACKET_ORDERED)
				{ 
					const unsigned __int64 orderingPredecessor = predecessorSequence;
					if (orderingPredecessor >= receivedSerialWindowStart && !processedSerials.get(orderingPredecessor))
					{

						IntrusivePtr<PacketBuffer> old;
						auto it = deferred.find(orderingPredecessor);
						if (it != deferred.end())
							old = it->second;
						deferred[orderingPredecessor] = msg;
						++deferredMessages;
						deferredBytes += msg->payloadLength();

						msg->next = old; 
						stateMutex.unlock();
						return;
					}
				}
				deliverReliablePacket(msg.GetRef());
				return;
			}
		}
		
		stateMutex.unlock();
		if (msg->packetEventHandler)
			msg->subscribedPacketEvent = (*msg->packetEventHandler)(msg.GetRef(), PacketInputReceived, msg->packetEventContext);
	}

	virtual void dataSent(size_t size, unsigned __int64 packetActivityMs)
	{
		if (packetActivityMs - sendRateSamples[sendRateWriteIndex] < transportTuning.retransmitHistoryMs * (1.0f / (SendRateSampleSlots - 8)))
		{
			sendRateSamples[sendRateWriteIndex + 1] += size;
		}
		else
		{
			unsigned __int64 total = sendRateSamples[sendRateWriteIndex + 1];
			if ((sendRateWriteIndex += 2) >= SendRateSampleSlots)
				sendRateWriteIndex = 0;
			sendRateSamples[sendRateWriteIndex] = packetActivityMs;
			sendRateSamples[sendRateWriteIndex + 1] = total + size;
		}
	}

	virtual void dataSentAck(size_t size)
	{
		stateMutex.lock();
		unsigned __int64 now = GetTickCount64();
		dataSent(size, now);

		if ((now - ackRateSamples[ackRateWriteIndex]) < ACK_RATE_SAMPLE_INTERVAL_MS)
		{
			ackRateSamples[ackRateWriteIndex + 1] += newBytes;
		}
		else
		{
			unsigned __int64 total = ackRateSamples[ackRateWriteIndex + 1];
			if ((ackRateWriteIndex += 2) >= AckRateSampleSlots)
				ackRateWriteIndex = 0;
			ackRateSamples[ackRateWriteIndex] = now;
			ackRateSamples[ackRateWriteIndex + 1] = total + newBytes;
		}
		stateMutex.unlock();
	}

	virtual void queueOutboundPacket(PacketBuffer* msg, bool urgent = false);

	virtual PacketBuffer* lastReliablePacket(bool urgent);

	virtual bool prepareNextPacket();

	virtual unsigned __int64 beginSendBatch(unsigned __int64 bunchStart);

	virtual void finishSendBatch();

	virtual unsigned __int64 packetSendTime(unsigned __int64 ser);

	static const unsigned __int64 RetransmitScanIntervalMs;

	virtual void discardPendingPackets()
	{
		stateMutex.lock();
		IntrusivePtr<PacketBuffer> ptr;
		IntrusivePtr<PacketBuffer> tmp;
		lastPriorityPacket = NULL;
		ptr = priorityQueueHead;
		priorityQueueHead = NULL;
		while (ptr)
		{
			tmp = ptr->next;
			ptr->cancelPacket();
			ptr = tmp;
		}
		lastReliableQueued = NULL;
		ptr = reliableQueueHead;
		reliableQueueHead = NULL;
		while (ptr)
		{
			tmp = ptr->next;
			ptr->cancelPacket();
			ptr = tmp;
		}
		ptr = bestEffortQueueHead;
		bestEffortQueueHead = NULL;
		while (ptr)
		{
			tmp = ptr->next;
			ptr->cancelPacket();
			ptr = tmp;
		}
		stateMutex.unlock();
	}

	virtual void updateLiveness(unsigned __int64 now)
	{
		if (!opened)
			return;
		stateMutex.lock();
		updateLivenessLocked(now);
		stateMutex.unlock();
	}

	virtual bool hasTimedOut()
	{
		stateMutex.lock();
		unsigned __int64 now = GetTickCount64();
		bool drop = serial >= MAX_LOCAL_SEQUENCE ||
			(now > lastPingReceivedMs + (PING_REPLY_TIMEOUT_MS << 1) && now > lastPacketReceivedMs + 15000);
		stateMutex.unlock();

		return drop;
	}

	virtual void serviceChannel();

	virtual void closeTransport()
	{
		if (!opened)
			return;
		opened = false;

		discardPendingPackets();
		if (peer)
		{
			if (!control)
				peer->detachChannel(this);
			peer = NULL;
		}
		stateMutex.lock();
		subsets.clear();
		receiveCallback = NULL;
		deferred.clear();
		deferredMessages = 0;
		deferredBytes = 0;
		sentPacketsBySequence.clear();

		stateMutex.unlock();
		if (PacketCache::sharedPacketCache())
			PacketCache::sharedPacketCache()->reclaimIdlePackets();
	}

	static void* operator new(size_t size) { return malloc(size); }

	static void* operator new(size_t size, const char* file, __int32 line) { return malloc(size); }

	static void operator delete(void* mem) { free(mem); }

	virtual ~ReliableChannel() { closeTransport(); }

protected:
	bool opened; 

	struct sockaddr_in dist; 

	unsigned __int64 serial;
	unsigned __int64 lastSentSequence;

	IntrusivePtr<PacketBuffer> reliableQueueHead; 
	PacketBuffer* reliableQueueTail;  
	IntrusivePtr<PacketBuffer> lastReliableQueued;   

	IntrusivePtr<PacketBuffer> priorityQueueHead; 
	PacketBuffer* priorityQueueTail;  
	IntrusivePtr<PacketBuffer> lastPriorityPacket;	  

	IntrusivePtr<PacketBuffer> bestEffortQueueHead; 
	PacketBuffer* bestEffortQueueTail;  

	SequenceBitmap pendingAckSerials; 

	unsigned __int64 ackMax;

	unsigned __int64 ackMin;

	SequenceBitmap recentPendingAckSerials; 

	unsigned __int64 sendRateSamples[SendRateSampleSlots];

	__int32 sendRateWriteIndex;

	void resetSendRateSamples(unsigned __int64 time);
	
	typedef std::unordered_map<unsigned __int64, IntrusivePtr<PacketBuffer>> SentPacketIndex;
	SentPacketIndex sentPacketsBySequence;
	
	unsigned __int64 lastRetransmitScanMs;
	
	void writeAcknowledgements(PacketBuffer* msg);
	
	void enqueueRetransmission(PacketBuffer* msg);
	
	void enqueuePriority(PacketBuffer* msg);
	
	void enqueuePriorityAfter(PacketBuffer* msg, PacketBuffer* after);

	void enqueueReliable(PacketBuffer* msg);
	
	void enqueueBestEffort(PacketBuffer* msg);
	
	bool preparePriorityPacket();
	
	bool prepareReliablePacket();
	
	bool prepareBestEffortPacket();

	std::unordered_map<unsigned __int64, IntrusivePtr<PacketBuffer>> deferred;
	unsigned deferredMessages = 0;
	size_t deferredBytes = 0;

	void updateReceiveMetrics(PacketBuffer* msg);

	void recordRetransmission(PacketBuffer* msg)
	{
		unsigned __int64 ser = msg->sequenceNumber();
		if (ser < receivedSerialWindowStart)
			return;
		if (ser >= ackRingFirstSequence)
		{
			
			__int32 serI = ackRingCursor - static_cast<int>(highestReceivedSequence - ser);
			if (serI < 0)
				serI += ACK_RING_CAPACITY;
			ackTime[serI] = (unsigned)msg->packetActivityMs;
			if (msg->header->flags & PACKET_RELIABLE)
				ack[serI] = transportTuning.ackRepeatCount; 
		}
		else
		{
			
			__int32 newIndex = recentAckTail + 1;
			if (newIndex >= RECENT_ACK_CAPACITY)
				newIndex = 0;
			if (newIndex != recentAckHead)
			{ 
				recentAckQueue[recentAckTail] = ser;
				recentAckTail = newIndex;
			}
		}
	}

	void deliverReliablePacket(PacketBuffer* msg)
	{
		// A reordered reliable chain can be thousands of messages long.
		// Drain it iteratively instead of recursing on the receive thread stack.
		std::vector<IntrusivePtr<PacketBuffer>> ready;
		ready.emplace_back(msg);
		while (!ready.empty())
		{
			IntrusivePtr<PacketBuffer> current = ready.back(); ready.pop_back();
			const unsigned __int64 ser = current->sequenceNumber();
			if (ser >= receivedSerialWindowStart)
			{
				if (current->packetEventHandler)
				{
					stateMutex.unlock();
					current->subscribedPacketEvent = (*current->packetEventHandler)(current.GetRef(), PacketInputReceived, current->packetEventContext);
					stateMutex.lock();
				}
				processedSerials.on(ser);
			}
			auto it = deferred.find(ser);
			if (it == deferred.end()) continue;
			IntrusivePtr<PacketBuffer> next = it->second; deferred.erase(it);
			while (next)
			{
				IntrusivePtr<PacketBuffer> item = next; next = item->next; item->next = NULL;
				--deferredMessages;
				deferredBytes -= item->payloadLength();
				ready.push_back(item);
			}
		}
		stateMutex.unlock();
	}

	void queueDelayedAcknowledgement(PacketBuffer* request);

	unsigned char ack[ACK_RING_CAPACITY];

	unsigned __int64 recentAckQueue[RECENT_ACK_CAPACITY];

	__int32 recentAckHead;
	
	__int32 recentAckTail;

	unsigned ackTime[ACK_RING_CAPACITY];

	__int32 ackRingCursor; 

	unsigned __int64 highestReceivedSequence;

	unsigned __int64 ackRingFirstSequence;

	SequenceBitmap receivedSerials; 

	unsigned __int64 receivedSerialWindowStart;

	bool starvation; 

	unsigned reliablePacketsSinceAck; 

	inline bool alreadyReceived(unsigned __int64 ser)
	{
		if (ser < receivedSerialWindowStart)
			return true;
		return receivedSerials.get(ser);
	}

	SequenceBitmap processedSerials; 

	unsigned newAcks; 

	unsigned newBytes; 
	
	void recordAcknowledgement(unsigned __int64 s, PacketBuffer* msg)
	{
		pendingAckSerials.off(s);
		recentPendingAckSerials.off(s);
		if (!msg)
			return;

		msg->status = PacketOutputAck;
		
		newAcks++;
		newBytes += msg->header->length + IPV4_UDP_HEADER_BYTES;
		
		if (msg->packetEventHandler && (msg->subscribedPacketEvent == PacketOutputTimeout || msg->subscribedPacketEvent == PacketOutputAck))
		{
			stateMutex.unlock();
			msg->subscribedPacketEvent = (*msg->packetEventHandler)(msg, PacketOutputAck, msg->packetEventContext);
			stateMutex.lock();
		}
	}

	unsigned __int64 lastPacketReceivedMs;

	unsigned __int64 lastPacketSentMs;

	unsigned __int64 lastPingReceivedMs;

	unsigned __int64 lastPingSentMs;

	unsigned __int64 lastProbePairSentMs;

	void updateLivenessLocked(unsigned __int64 now)
	{
		if (control)
			return; 

		if (!now)
			now = GetTickCount64();
		if (!starvation && 
			(!reliablePacketsSinceAck ||
				now < lastPacketSentMs + (heartbeatIntervalMs >> 1)) && 
			(now < lastPingReceivedMs + PING_REPLY_TIMEOUT_MS ||			 
				now < lastPingSentMs + PING_RETRY_INTERVAL_MS) &&	 
			now < lastPacketSentMs + heartbeatIntervalMs)				 
			return;

		IntrusivePtr<PacketBuffer> msg = priorityQueueHead; 
		const unsigned msgLen = sizeof(DatagramHeader);

		if (!msg)
		{
			msg = PacketCache::sharedPacketCache()->acquirePacket(msgLen - sizeof(DatagramHeader), this);
			msg->resizePayload(msgLen - sizeof(DatagramHeader));
			msg->header->flags |= PACKET_HEADER_ONLY;
			enqueuePriority(msg.GetRef());
		}

		if (now > lastPingSentMs + PING_RETRY_INTERVAL_MS)
		{

			msg->header->flags |= PACKET_PING_REQUEST;
			lastPingSentMs = now;
		}

		starvation = false;
		lastPacketSentMs = now;
	}

	unsigned heartbeatIntervalMs;
	unsigned smoothedRttMs;
	unsigned latestRttMs;
	unsigned __int64 ackRateSamples[AckRateSampleSlots];
	__int32 ackRateWriteIndex;

	float estimateAcknowledgedRate(unsigned __int64 windowSize);
};

#endif
