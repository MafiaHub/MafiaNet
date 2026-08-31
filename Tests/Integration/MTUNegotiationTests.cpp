/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string.h>
#include <vector>

#include "mafianet/peer.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/MTUSize.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"

#ifdef _WIN32
#include <ws2tcpip.h> // inet_pton, htons (winsock2 pulled in via peer.h)
typedef SOCKET RawSocket;
typedef int RawSockLen;
#define CLOSE_RAW_SOCKET closesocket
#define INVALID_RAW_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int RawSocket;
typedef socklen_t RawSockLen;
#define CLOSE_RAW_SOCKET close
#define INVALID_RAW_SOCKET (-1)
#endif

using namespace MafiaNet;

/*
Description:
Covers the MTU the connection handshake settles on, and the connect loop that probes for it.

The handshake pads ID_OPEN_CONNECTION_REQUEST_1 down a ladder of sizes and keeps the largest one
the path passed. That value is then frozen for the life of the connection and used to size every
datagram in BOTH directions -- nothing re-probes, and nothing detects a path-MTU black hole
afterwards -- so two properties matter, and neither is visible without a real connection:

- Both peers must end up on the same size, and that size must be inside [MINIMUM, MAXIMUM]. A
  value above MAXIMUM_MTU_SIZE would have the reliability layer build datagrams larger than the
  buffers it builds them into.
- Splitting and reassembly must still work at whatever that size is. Changing MAXIMUM_MTU_SIZE
  changes the fragment size of every message bigger than one datagram.

Success conditions:
- A loopback connection negotiates exactly MAXIMUM_MTU_SIZE (loopback passes the top rung), and
  both sides report it.
- A message many times the MTU arrives byte-for-byte intact.
- Connect() works when asked for fewer attempts than there are rungs in the ladder.

Failure conditions: any of the above does not hold.
*/

namespace
{
	const int kConnectTimeoutMs = 15000;
	const int kTransferTimeoutMs = 20000;

	// Drain a peer, returning the first packet with the given id, or 0 if the deadline passes.
	// Packets that are not the wanted id are discarded; both peers are pumped so a handshake that
	// needs traffic from either side can make progress.
	Packet *PumpUntil(RakPeerInterface *wanted, int wantedId, RakPeerInterface *alsoPump, int timeoutMs)
	{
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)timeoutMs)
		{
			Packet *p;
			for (p = wanted->Receive(); p; wanted->DeallocatePacket(p), p = wanted->Receive())
			{
				if (p->data[0] == (unsigned char)wantedId)
					return p; // caller deallocates
			}
			if (alsoPump)
			{
				for (p = alsoPump->Receive(); p; alsoPump->DeallocatePacket(p), p = alsoPump->Receive())
					;
			}
			RakSleep(15);
		}
		return 0;
	}

	// Wait until each peer has reported its own connection packet, capturing the address each saw.
	//
	// This has to be a single loop over both peers rather than two PumpUntil calls. PumpUntil
	// discards every packet that is not the one it is waiting for, so waiting on the client first
	// throws away the server's ID_NEW_INCOMING_CONNECTION when it happens to arrive during that
	// wait, and the second call then blocks until its deadline. That is a real flake, not a
	// theoretical one -- it took 15 repeats to surface.
	bool PumpUntilBothConnected(RakPeerInterface *client, RakPeerInterface *server, SystemAddress *serverAddressOut, SystemAddress *clientAddressOut, int timeoutMs)
	{
		bool clientSaw = false;
		bool serverSaw = false;
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)timeoutMs)
		{
			Packet *p;
			for (p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
			{
				if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				{
					if (serverAddressOut)
						*serverAddressOut = p->systemAddress;
					clientSaw = true;
				}
			}
			for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
			{
				if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
				{
					if (clientAddressOut)
						*clientAddressOut = p->systemAddress;
					serverSaw = true;
				}
			}
			if (clientSaw && serverSaw)
				return true;
			RakSleep(15);
		}
		return false;
	}

	// Poll the closing side's own view of a connection until its slot is gone.
	//
	// CloseConnection is asynchronous, and the server's ID_DISCONNECTION_NOTIFICATION says nothing
	// about when the closing peer finishes its own teardown. Connect() refuses an address that still
	// holds an active slot with ALREADY_CONNECTED_TO_ENDPOINT, so a test that closes and reconnects
	// has to wait for this and not for the notification. IS_DISCONNECTED means the slot is no longer
	// active and IS_NOT_CONNECTED means it is gone entirely; both clear that gate.
	bool PumpUntilDisconnected(RakPeerInterface *peer, const SystemAddress &address, RakPeerInterface *alsoPump, int timeoutMs)
	{
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)timeoutMs)
		{
			Packet *p;
			for (p = peer->Receive(); p; peer->DeallocatePacket(p), p = peer->Receive())
				;
			if (alsoPump)
			{
				for (p = alsoPump->Receive(); p; alsoPump->DeallocatePacket(p), p = alsoPump->Receive())
					;
			}
			const ConnectionState state = peer->GetConnectionState(address);
			if (state == IS_NOT_CONNECTED || state == IS_DISCONNECTED)
				return true;
			RakSleep(15);
		}
		return false;
	}

	// Mirrors the constant of the same name in RakPeer.cpp, which is file-static and so cannot be
	// included. It is the marker that makes a receiving peer treat a datagram as an offline
	// (pre-connection) message rather than reliability-layer traffic. Duplicated deliberately: a
	// test for what an unconforming peer can put on the wire should pin the wire format itself.
	const unsigned char kOfflineMessageDataId[16] =
		{0x00,0xFF,0xFF,0x00,0xFE,0xFE,0xFE,0xFE,0xFD,0xFD,0xFD,0xFD,0x12,0x34,0x56,0x78};

	// Returned by PumpUntilMTUAssigned when no slot ever appeared.
	const int kNoMTUAssigned = -1;

	// A plain UDP socket, standing in for a peer that does not run this library. The MTU in the
	// handshake is just a number on the wire, so sending a value no conforming peer would send is
	// the only way to reach the receiving side's clamp.
	//
	// Winsock needs no setup here: RakPeer's constructor holds a WSAStartup reference for as long
	// as a peer exists, and the fixture creates both peers in SetUp.
	class RawUdpSocket
	{
	public:
		RawUdpSocket() : fd(INVALID_RAW_SOCKET), port(0) {}
		~RawUdpSocket() { Close(); }

		bool Open()
		{
			fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (fd == INVALID_RAW_SOCKET)
				return false;

			sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = 0; // OS-assigned, like every other port in this suite
			if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
				return false;
			if (bind(fd, (const sockaddr *)&addr, sizeof(addr)) != 0)
				return false;

			sockaddr_in bound;
			memset(&bound, 0, sizeof(bound));
			RawSockLen boundLen = (RawSockLen)sizeof(bound);
			if (getsockname(fd, (sockaddr *)&bound, &boundLen) != 0)
				return false;
			port = ntohs(bound.sin_port);
			return true;
		}

		bool SendTo(BitStream &bs, unsigned short destPort)
		{
			sockaddr_in dest;
			memset(&dest, 0, sizeof(dest));
			dest.sin_family = AF_INET;
			dest.sin_port = htons(destPort);
			if (inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr) != 1)
				return false;

			const int length = (int)bs.GetNumberOfBytesUsed();
			return sendto(fd, (const char *)bs.GetData(), length, 0, (const sockaddr *)&dest, sizeof(dest)) == length;
		}

		void Close()
		{
			if (fd != INVALID_RAW_SOCKET)
			{
				CLOSE_RAW_SOCKET(fd);
				fd = INVALID_RAW_SOCKET;
			}
		}

		unsigned short Port() const { return port; }

	private:
		RawSocket fd;
		unsigned short port;
	};

	// Build the ID_OPEN_CONNECTION_REQUEST_2 an accepting peer parses, with a caller-chosen MTU.
	// Written with the same BitStream calls the real handshake uses, so the encoding of the address
	// and GUID matches whatever those types currently serialise to.
	void WriteOpenConnectionRequest2(BitStream &out, const SystemAddress &bindingAddress, uint16_t mtu, const RakNetGUID &guid)
	{
		out.Write((MessageID)ID_OPEN_CONNECTION_REQUEST_2);
		out.WriteAlignedBytes(kOfflineMessageDataId, sizeof(kOfflineMessageDataId));
		out.Write(bindingAddress);
		out.Write(mtu);
		out.Write(guid);
	}

	// Poll until the peer has given this address a slot, and return that slot's MTU.
	//
	// An address with no slot reports defaultMTUSize, which is exactly what GetMTUSize() returns for
	// the sentinel, so that value doubles as "not yet". Callers must therefore not forge an MTU
	// equal to defaultMTUSize -- there would be no way to tell the two apart.
	int PumpUntilMTUAssigned(RakPeerInterface *peer, const SystemAddress &address, int timeoutMs)
	{
		const int unassigned = peer->GetMTUSize(UNASSIGNED_SYSTEM_ADDRESS);
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)timeoutMs)
		{
			Packet *p;
			for (p = peer->Receive(); p; peer->DeallocatePacket(p), p = peer->Receive())
				;
			const int mtu = peer->GetMTUSize(address);
			if (mtu != unassigned)
				return mtu;
			RakSleep(15);
		}
		return kNoMTUAssigned;
	}

	class MTUNegotiation : public ::testing::Test
	{
	public:
		void SetUp() override
		{
			server = RakPeerInterface::GetInstance();
			client = RakPeerInterface::GetInstance();
			ASSERT_NE(server, nullptr);
			ASSERT_NE(client, nullptr);
		}

		void TearDown() override
		{
			// In TearDown rather than after the assertions, so a failed ASSERT_ still destroys the
			// peers: a leaked peer keeps a socket bound and a network thread alive for the rest of
			// the process, which in a serial suite surfaces as an unrelated later test failing.
			if (client)
			{
				client->Shutdown(100);
				RakPeerInterface::DestroyInstance(client);
				client = 0;
			}
			if (server)
			{
				server->Shutdown(100);
				RakPeerInterface::DestroyInstance(server);
				server = 0;
			}
		}

		// Start both peers on OS-assigned ports and return the server's port.
		unsigned short StartPeers()
		{
			SocketDescriptor serverSd(0, "127.0.0.1");
			EXPECT_EQ(server->Startup(8, &serverSd, 1), RAKNET_STARTED);
			server->SetMaximumIncomingConnections(8);

			SocketDescriptor clientSd(0, "127.0.0.1");
			EXPECT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);

			return server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
		}

		RakPeerInterface *server = 0;
		RakPeerInterface *client = 0;
	};
} // namespace

// The negotiated MTU is applied to both directions, so both peers must hold the same number and it
// must be one the datagram buffers can hold. Loopback passes the top rung, so the expected value is
// exact -- which also proves the ladder is not silently starting below MAXIMUM_MTU_SIZE.
//
// This cannot prove the clamp on a remote peer's reported MTU: both peers here are built against
// the same MAXIMUM_MTU_SIZE, so neither can name a larger one through the public API. What it does
// catch is the negotiated value drifting out of range for any other reason.
TEST_F(MTUNegotiation, LoopbackNegotiatesTheMaximumOnBothSides)
{
	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	SystemAddress serverAddress;
	SystemAddress clientAddress;
	ASSERT_TRUE(PumpUntilBothConnected(client, server, &serverAddress, &clientAddress, kConnectTimeoutMs))
		<< "both peers did not report the connection";

	const int clientSideMTU = client->GetMTUSize(serverAddress);
	const int serverSideMTU = server->GetMTUSize(clientAddress);

	EXPECT_EQ(clientSideMTU, MAXIMUM_MTU_SIZE);
	EXPECT_EQ(serverSideMTU, MAXIMUM_MTU_SIZE);
	EXPECT_EQ(clientSideMTU, serverSideMTU) << "the two peers disagree about the datagram size";
	EXPECT_GE(clientSideMTU, MINIMUM_MTU_SIZE);
}

// Splitting and reassembly happen at the negotiated MTU, so changing that number changes the
// fragment size of every message larger than one datagram. Send one many times the MTU and check it
// back byte for byte.
TEST_F(MTUNegotiation, MessageManyTimesTheMTUArrivesIntact)
{
	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	SystemAddress serverAddress;
	ASSERT_TRUE(PumpUntilBothConnected(client, server, &serverAddress, 0, kConnectTimeoutMs))
		<< "both peers did not report the connection";

	// ~30 fragments at the current MTU. Deterministic contents, so a reassembly that reorders or
	// drops a fragment fails on the comparison rather than only on the length.
	const size_t payloadSize = (size_t)MAXIMUM_MTU_SIZE * 30;
	std::vector<char> payload(payloadSize);
	for (size_t i = 0; i < payloadSize; ++i)
		payload[i] = (char)((i * 31 + (i >> 8)) & 0xFF);

	BitStream bs;
	bs.Write((MessageID)(ID_USER_PACKET_ENUM + 1));
	bs.Write(payload.data(), (unsigned int)payloadSize);
	ASSERT_NE(client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false), 0u);

	Packet *received = PumpUntil(server, ID_USER_PACKET_ENUM + 1, client, kTransferTimeoutMs);
	ASSERT_NE(received, nullptr) << "the split message never reassembled";
	ASSERT_EQ((size_t)received->length, payloadSize + sizeof(MessageID));
	EXPECT_EQ(memcmp(received->data + sizeof(MessageID), payload.data(), payloadSize), 0);
	server->DeallocatePacket(received);
}

// Regression: the connect loop divided sendConnectionAttemptCount by the number of rungs in the MTU
// ladder to decide how many attempts to spend on each. Connect() takes that count from the caller
// and validates nothing, so any value below the rung count divided by zero on the first tick of the
// network thread. It was unreachable only because the default (12) happened to exceed the ladder --
// and adding a rung is exactly the kind of change that makes a latent crash like this reachable.
TEST_F(MTUNegotiation, ConnectWorksWithFewerAttemptsThanMTURungs)
{
	const unsigned short port = StartPeers();

	// One, two and three are all below the rung count. The first attempt uses the top rung, which
	// loopback passes, so the connection completes however small the budget is.
	for (unsigned attempts = 1; attempts <= 3; ++attempts)
	{
		SCOPED_TRACE(testing::Message() << "sendConnectionAttemptCount=" << attempts);

		ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0, 0, 0, attempts, 500, 0), CONNECTION_ATTEMPT_STARTED);

		SystemAddress serverAddress;
		ASSERT_TRUE(PumpUntilBothConnected(client, server, &serverAddress, 0, kConnectTimeoutMs))
			<< "both peers did not report the connection";

		EXPECT_EQ(client->GetMTUSize(serverAddress), MAXIMUM_MTU_SIZE);

		client->CloseConnection(serverAddress, true);
		Packet *closed = PumpUntil(server, ID_DISCONNECTION_NOTIFICATION, client, kConnectTimeoutMs);
		ASSERT_NE(closed, nullptr) << "the server never saw the connection close";
		server->DeallocatePacket(closed);

		// The server's notification does not mean the client has finished tearing its own slot down,
		// and the next iteration's Connect() would be refused while it has not.
		ASSERT_TRUE(PumpUntilDisconnected(client, serverAddress, server, kConnectTimeoutMs))
			<< "the client never released its connection slot";
	}
}

// The MTU an accepting peer adopts arrives in ID_OPEN_CONNECTION_REQUEST_2, which is unauthenticated
// wire data: nothing but the clamp stops a peer naming any uint16 it likes. That matters because the
// value sizes every datagram the reliability layer builds into buffers declared MAXIMUM_MTU_SIZE
// bytes long, so an unclamped one writes past the end of them.
//
// Two conforming peers cannot produce this -- both are built against the same MAXIMUM_MTU_SIZE and
// the connecting side never pads above it -- so the packet has to come from a socket that is not
// running this library.
//
// The reliability layer cannot end up disagreeing with the value asserted here:
// AssignSystemAddressToRemoteSystemList hands MTUSize to reliabilityLayer.Reset() on the next line,
// with no second path that could set one without the other.
TEST_F(MTUNegotiation, ForgedOversizedMTUFromTheWireIsClamped)
{
	const unsigned short port = StartPeers();

	RawUdpSocket forger;
	ASSERT_TRUE(forger.Open());

	SystemAddress serverAddress;
	ASSERT_TRUE(serverAddress.FromStringExplicitPort("127.0.0.1", port));

	BitStream forged;
	WriteOpenConnectionRequest2(forged, serverAddress, 0xFFFF, RakNetGUID(0x00C0FFEE00C0FFEEull));
	ASSERT_TRUE(forger.SendTo(forged, port));

	SystemAddress forgedAddress;
	ASSERT_TRUE(forgedAddress.FromStringExplicitPort("127.0.0.1", forger.Port()));

	const int mtu = PumpUntilMTUAssigned(server, forgedAddress, kConnectTimeoutMs);
	ASSERT_NE(mtu, kNoMTUAssigned) << "the server never accepted the forged handshake packet";
	EXPECT_EQ(mtu, MAXIMUM_MTU_SIZE) << "a peer-supplied MTU of 0xFFFF was not clamped";
}

// The other half of the clamp: a size the buffers can hold must survive untouched, or the fix would
// be silently capping every connection at the wrong number. 1024 is a real rung of the ladder, and
// is deliberately not defaultMTUSize -- see PumpUntilMTUAssigned.
TEST_F(MTUNegotiation, ForgedInRangeMTUIsKept)
{
	const unsigned short port = StartPeers();

	RawUdpSocket forger;
	ASSERT_TRUE(forger.Open());

	SystemAddress serverAddress;
	ASSERT_TRUE(serverAddress.FromStringExplicitPort("127.0.0.1", port));

	BitStream forged;
	WriteOpenConnectionRequest2(forged, serverAddress, 1024, RakNetGUID(0x00DEFACED00DEFACull));
	ASSERT_TRUE(forger.SendTo(forged, port));

	SystemAddress forgedAddress;
	ASSERT_TRUE(forgedAddress.FromStringExplicitPort("127.0.0.1", forger.Port()));

	const int mtu = PumpUntilMTUAssigned(server, forgedAddress, kConnectTimeoutMs);
	ASSERT_NE(mtu, kNoMTUAssigned) << "the server never accepted the forged handshake packet";
	EXPECT_EQ(mtu, 1024) << "an MTU within the limit was altered";
}
