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

	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr) << "client never reported a connection";
	const SystemAddress serverAddress = accepted->systemAddress;
	client->DeallocatePacket(accepted);

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr) << "server never reported a connection";
	const SystemAddress clientAddress = incoming->systemAddress;
	server->DeallocatePacket(incoming);

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

	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr) << "client never reported a connection";
	const SystemAddress serverAddress = accepted->systemAddress;
	client->DeallocatePacket(accepted);

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr) << "server never reported a connection";
	server->DeallocatePacket(incoming);

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

		Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
		ASSERT_NE(accepted, nullptr) << "client never reported a connection";
		const SystemAddress serverAddress = accepted->systemAddress;
		client->DeallocatePacket(accepted);

		EXPECT_EQ(client->GetMTUSize(serverAddress), MAXIMUM_MTU_SIZE);

		Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
		ASSERT_NE(incoming, nullptr) << "server never reported a connection";
		server->DeallocatePacket(incoming);

		client->CloseConnection(serverAddress, true);
		Packet *closed = PumpUntil(server, ID_DISCONNECTION_NOTIFICATION, client, kConnectTimeoutMs);
		ASSERT_NE(closed, nullptr) << "the server never saw the connection close";
		server->DeallocatePacket(closed);
	}
}
