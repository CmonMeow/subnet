#ifdef _MSC_VER
#pragma once
#endif

#ifndef NETGLOBAL_H
#define NETGLOBAL_H

#pragma pack(push, netGlobal, 1)

#define IPV4_HOST_ORDER(addr) ntohl((addr).sin_addr.S_un.S_addr)

#define IPV4_OCTET_1(addr) ((addr).sin_addr.S_un.S_un_b.s_b1)

#define IPV4_OCTET_2(addr) ((addr).sin_addr.S_un.S_un_b.s_b2)

#define IPV4_OCTET_3(addr) ((addr).sin_addr.S_un.S_un_b.s_b3)

#define IPV4_OCTET_4(addr) ((addr).sin_addr.S_un.S_un_b.s_b4)

#define UDP_PORT_HOST_ORDER(addr) ntohs((addr).sin_port)

const __int32 DatagramReceiveBufferBytes = 2048;
const unsigned DatagramAuthenticationTagBytes = 16;
const unsigned MAX_REASSEMBLED_USER_MESSAGE = 65536;

struct DatagramHeader {
	
	unsigned short length;
	
	unsigned short flags;

	unsigned __int32 crc;
	
	unsigned __int32 serial;
	
	unsigned __int32 ackBaseSequence;
	// Packet flags select the view; all three occupy the same eight bytes.
	union
	{
		unsigned __int64 ackBits;
		struct
		{
			unsigned __int32 ackBits;
			unsigned __int32 predecessorSequence;
		} ordered;
		struct
		{
			unsigned __int32 ackBits;
			unsigned __int32 replyDelayMs;
		} pingReply;
	};
};

#define PACKET_RELIABLE 0x8000

#define PACKET_PRIORITY 0x4000

#define PACKET_ORDERED 0x2000

#define PACKET_FROM_CONTROL_CHANNEL 0x1000

#define PACKET_TO_CONTROL_CHANNEL 0x0800

#define PACKET_PING_REPLY 0x0400

#define PACKET_PING_REQUEST 0x0200

#define PACKET_BATCH_MEMBER 0x0080

#define PACKET_HEADER_ONLY 0x0040

#define PACKET_FRAGMENT 0x0020

#define PACKET_FINAL_FRAGMENT 0x0010

#define PACKET_VOICE 0x0008

#define PACKET_APP_FLAG_MASK 0x0007

#define PACKET_FLAG_MASK 0xffff

#define PACKET_HAS_SHORT_ACK(flags) (((flags) & (PACKET_ORDERED | PACKET_PING_REPLY)) != 0)

#define IPV4_HEADER_BYTES 20
#define UDP_HEADER_BYTES 8
#define TCP_HEADER_BYTES 20
#define IPV4_UDP_HEADER_BYTES 28
// Reserve the authentication tag inside the existing 1400-byte IPv4 packet budget.
#define DATAGRAM_PAYLOAD_LIMIT_BYTES (1400 - IPV4_UDP_HEADER_BYTES - sizeof(DatagramHeader) - DatagramAuthenticationTagBytes)

enum PacketStatus
{
	PacketError, 
	PacketOK,	 

	PacketInvalidSharing, 
	PacketInvalidMessage, 

	PacketInputPending,	   
	PacketInputReceived,   
	PacketInputPartialAck, 
	PacketInputAck,		   

	PacketOutputPending,  
	PacketOutputSent,	  
	PacketOutputObsolete, 
	PacketOutputTimeout,  
	PacketOutputAck,	  

	PacketCancel,		   
	PacketNoMoreCallbacks, 
};

class PacketBuffer;

typedef PacketStatus PacketCallback(PacketBuffer* msg, PacketStatus event, void* data);

#pragma pack(pop, netGlobal)

#endif
