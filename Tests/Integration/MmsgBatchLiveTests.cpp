/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Live traffic tests for batched datagram I/O (recvmmsg / sendmmsg).
 *
 *  These exist because the rest of the suite never fills a batch. The heaviest
 *  existing test sends one packet per 30 ms tick, so the reliability layer has a
 *  single datagram in flight per UpdateInternal and RNS2SendBatch flushes a batch
 *  of one every time -- the coalescing path the feature exists for is never
 *  reached, and neither is the mid-loop flush at MMSG_BATCH_MAX. The tests below
 *  deliberately queue far more datagrams per tick than MMSG_BATCH_MAX so that
 *  path runs, and assert the stream survives it byte-for-byte and in order.
 *
 *  They are meaningful on every platform: off Linux they pin the portable
 *  per-datagram path's behaviour as the reference, and on Linux the identical
 *  assertions run against the batched paths. A divergence between the two shows
 *  up as a failure here rather than as corrupted traffic in production.
 */

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/PeerHandle.h" // Peer, PacketPtr (RAII: survives a failed ASSERT)
#include "mafianet/GetTime.h"
#include "mafianet/sleep.h"
#include "mafianet/MmsgBatch.h" // MMSG_BATCH_MAX

#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
	// Payload large enough that each message becomes its own datagram rather than
	// being coalesced with its neighbours into one. Comfortably under the MTU so
	// nothing splits either -- a split message would be several datagrams for one
	// sequence number and blunt the ordering assertions.
	const int PAYLOAD_BYTES = 900;

	// Enough messages that the send loop must cross the MMSG_BATCH_MAX boundary
	// many times over once congestion control opens up.
	const unsigned MESSAGE_COUNT = MMSG_BATCH_MAX * 40; // 2560

	const unsigned char ID_BATCH_TEST = ID_USER_PACKET_ENUM + 1;

	// A message is its own checksum: a 4-byte sequence number followed by a body
	// derived from it. Any datagram that gets truncated, aliased against the next
	// one (the batch copies payloads because the caller reuses its serialization
	// buffer), or reordered fails a specific assertion rather than a vague count.
	void BuildMessage(std::vector<char> &out, uint32_t sequence)
	{
		out.assign((size_t) PAYLOAD_BYTES, 0);
		out[0] = (char) ID_BATCH_TEST;
		out[1] = (char) (sequence & 0xFF);
		out[2] = (char) ((sequence >> 8) & 0xFF);
		out[3] = (char) ((sequence >> 16) & 0xFF);
		out[4] = (char) ((sequence >> 24) & 0xFF);
		for (size_t i = 5; i < out.size(); ++i)
			out[i] = (char) ((sequence + i) & 0xFF);
	}

	// Returns false with a reason if the packet is not exactly the message that
	// BuildMessage would have produced for its embedded sequence number.
	bool VerifyMessage(const unsigned char *data, unsigned length, uint32_t *sequenceOut,
	                   std::string *reason)
	{
		if (length != (unsigned) PAYLOAD_BYTES)
		{
			*reason = "wrong length: got " + std::to_string(length) +
			          ", expected " + std::to_string(PAYLOAD_BYTES);
			return false;
		}
		const uint32_t sequence = (uint32_t) data[1] | ((uint32_t) data[2] << 8) |
		                          ((uint32_t) data[3] << 16) | ((uint32_t) data[4] << 24);
		std::vector<char> expected;
		BuildMessage(expected, sequence);
		if (memcmp(expected.data(), data, (size_t) PAYLOAD_BYTES) != 0)
		{
			*reason = "payload corrupted for sequence " + std::to_string(sequence);
			return false;
		}
		*sequenceOut = sequence;
		return true;
	}

	// Bring up a server and a client on OS-assigned ephemeral ports and wait for
	// the connection to be observed by both. Returns the server's port.
	bool ConnectLocally(Peer &server, Peer &client, unsigned maxConnections,
	                    unsigned short *serverPortOut, std::string *reason)
	{
		SocketDescriptor serverSd(0, "127.0.0.1");
		if (server->Startup(maxConnections, &serverSd, 1) != RAKNET_STARTED)
		{
			*reason = "server Startup failed";
			return false;
		}
		server->SetMaximumIncomingConnections((unsigned short) maxConnections);
		const unsigned short serverPort = server->GetInternalID().GetPort();

		SocketDescriptor clientSd(0, "127.0.0.1");
		if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
		{
			*reason = "client Startup failed";
			return false;
		}
		if (client->Connect("127.0.0.1", serverPort, 0, 0) != CONNECTION_ATTEMPT_STARTED)
		{
			*reason = "Connect did not start";
			return false;
		}

		const TimeMS deadline = GetTimeMS() + 15000;
		while (GetTimeMS() < deadline &&
		       !(server->NumberOfConnections() >= 1 && client->NumberOfConnections() >= 1))
		{
			for (auto pkt : server.incoming()) (void) pkt;
			for (auto pkt : client.incoming()) (void) pkt;
			RakSleep(15);
		}
		if (server->NumberOfConnections() < 1 || client->NumberOfConnections() < 1)
		{
			*reason = "peers did not connect within 15s";
			return false;
		}
		*serverPortOut = serverPort;
		return true;
	}
}

// Reliable-ordered burst large enough to fill many batches. Verifies the batched
// send path delivers every datagram, uncorrupted, in order.
TEST(MmsgBatchLive, LargeReliableOrderedBurstArrivesIntactAndInOrder)
{
	Peer server;
	RakSleep(2); // distinct GUID seeds; see the note in TypedSendLiveTests
	Peer client;

	unsigned short serverPort = 0;
	std::string reason;
	ASSERT_TRUE(ConnectLocally(server, client, 1, &serverPort, &reason)) << reason;

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);

	// Queue the whole burst before pumping. The reliability layer therefore has
	// thousands of datagrams outstanding when UpdateInternal next runs, so its
	// per-tick datagram loop runs far past MMSG_BATCH_MAX and RNS2SendBatch
	// flushes mid-loop -- the path a one-packet-per-tick test never reaches.
	std::vector<char> message;
	for (uint32_t i = 0; i < MESSAGE_COUNT; ++i)
	{
		BuildMessage(message, i);
		ASSERT_NE(client->Send(message.data(), PAYLOAD_BYTES, MafiaNet::Priority::High,
		                       MafiaNet::Reliability::ReliableOrdered, 0, serverAddress, false),
		          0u)
			<< "Send rejected message " << i;
	}

	// Drain until every message arrives or the deadline expires. Reliable-ordered
	// delivery is guaranteed, so a shortfall is a real failure, not a timing miss;
	// the deadline is generous purely so a slow machine does not report one.
	std::vector<bool> seen((size_t) MESSAGE_COUNT, false);
	unsigned received = 0;
	uint32_t nextExpected = 0;
	const TimeMS deadline = GetTimeMS() + 120000;

	while (GetTimeMS() < deadline && received < MESSAGE_COUNT)
	{
		for (auto pkt : server.incoming())
		{
			if (pkt->data[0] != ID_BATCH_TEST)
				continue; // connection bookkeeping

			uint32_t sequence = 0;
			std::string why;
			ASSERT_TRUE(VerifyMessage(pkt->data, pkt->length, &sequence, &why)) << why;
			ASSERT_LT(sequence, MESSAGE_COUNT) << "sequence " << sequence << " out of range";
			ASSERT_FALSE(seen[sequence]) << "duplicate delivery of message " << sequence;

			// ReliableOrdered on a single channel: sequence numbers must arrive
			// densely in order. A batch flushed out of order, or a datagram
			// silently dropped and never resent, breaks exactly here.
			ASSERT_EQ(sequence, nextExpected)
				<< "out-of-order delivery: expected " << nextExpected << ", got " << sequence;
			++nextExpected;

			seen[sequence] = true;
			++received;
		}
		for (auto pkt : client.incoming()) (void) pkt;
		RakSleep(5);
	}

	ASSERT_EQ(received, MESSAGE_COUNT)
		<< "only " << received << " of " << MESSAGE_COUNT
		<< " messages arrived; first missing sequence is " << nextExpected;
}

// Several clients bursting at one server simultaneously. This is the receive
// side: a single server socket draining datagrams from multiple sources is what
// makes recvmmsg return more than one message per call, and it is the case where
// a mixed-up source address would surface as traffic attributed to the wrong
// peer.
TEST(MmsgBatchLive, ConcurrentSendersAreEachAttributedToTheRightPeer)
{
	const unsigned CLIENT_COUNT = 3;
	const unsigned PER_CLIENT = MMSG_BATCH_MAX * 8; // 512 each

	Peer server;

	// Construct the peers one at a time with a gap between them, NOT as
	// std::vector<Peer>(CLIENT_COUNT): on POSIX the GUID is seeded from the
	// microsecond clock inside RakPeerInterface::GetInstance(), which the Peer
	// constructor calls. Peers built back-to-back draw identical GUIDs and the
	// server rejects the duplicates, so only some of the clients ever connect.
	// reserve() first so emplace_back never reallocates mid-loop.
	std::vector<Peer> clients;
	clients.reserve(CLIENT_COUNT);
	for (unsigned c = 0; c < CLIENT_COUNT; ++c)
	{
		RakSleep(2);
		clients.emplace_back();
	}

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(CLIENT_COUNT, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections((unsigned short) CLIENT_COUNT);
	const unsigned short serverPort = server->GetInternalID().GetPort();

	for (unsigned c = 0; c < CLIENT_COUNT; ++c)
	{
		SocketDescriptor clientSd(0, "127.0.0.1");
		ASSERT_EQ(clients[c]->Startup(1, &clientSd, 1), RAKNET_STARTED) << "client " << c;
		ASSERT_EQ(clients[c]->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED)
			<< "client " << c;
	}

	TimeMS deadline = GetTimeMS() + 20000;
	while (GetTimeMS() < deadline && server->NumberOfConnections() < CLIENT_COUNT)
	{
		for (auto pkt : server.incoming()) (void) pkt;
		for (unsigned c = 0; c < CLIENT_COUNT; ++c)
			for (auto pkt : clients[c].incoming()) (void) pkt;
		RakSleep(15);
	}
	ASSERT_EQ(server->NumberOfConnections(), CLIENT_COUNT) << "clients did not all connect";

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);

	// Each client's messages carry sequence numbers in its own disjoint range, so
	// the server can tell not just that everything arrived but that each datagram
	// came from the peer whose range it belongs to.
	std::vector<char> message;
	for (unsigned c = 0; c < CLIENT_COUNT; ++c)
	{
		for (unsigned i = 0; i < PER_CLIENT; ++i)
		{
			BuildMessage(message, c * PER_CLIENT + i);
			ASSERT_NE(clients[c]->Send(message.data(), PAYLOAD_BYTES, MafiaNet::Priority::High,
			                           MafiaNet::Reliability::ReliableOrdered, 0, serverAddress, false),
			          0u)
				<< "client " << c << " Send rejected message " << i;
		}
	}

	const unsigned TOTAL = CLIENT_COUNT * PER_CLIENT;
	std::vector<bool> seen((size_t) TOTAL, false);
	unsigned received = 0;
	deadline = GetTimeMS() + 120000;

	while (GetTimeMS() < deadline && received < TOTAL)
	{
		for (auto pkt : server.incoming())
		{
			if (pkt->data[0] != ID_BATCH_TEST)
				continue;

			uint32_t sequence = 0;
			std::string why;
			ASSERT_TRUE(VerifyMessage(pkt->data, pkt->length, &sequence, &why)) << why;
			ASSERT_LT(sequence, TOTAL) << "sequence " << sequence << " out of range";
			ASSERT_FALSE(seen[sequence]) << "duplicate delivery of message " << sequence;
			seen[sequence] = true;
			++received;

			// The sender's guid must match the range the sequence number falls in.
			// A source address decoded from the wrong slot of a recvmmsg batch
			// would land here.
			const unsigned owner = sequence / PER_CLIENT;
			ASSERT_EQ(pkt->guid, clients[owner]->GetMyGUID())
				<< "message " << sequence << " attributed to the wrong peer";
		}
		for (unsigned c = 0; c < CLIENT_COUNT; ++c)
			for (auto pkt : clients[c].incoming()) (void) pkt;
		RakSleep(5);
	}

	ASSERT_EQ(received, TOTAL) << "only " << received << " of " << TOTAL << " messages arrived";
}

// Sustained bidirectional traffic. The single-burst tests above drain a queue
// that was filled once; this one keeps both directions busy for a while, so the
// batch is repeatedly filled, flushed and refilled across many ticks rather than
// drained once. It is the closest thing in the suite to steady-state load, and
// it is where a slot leaked or double-freed by the recv carry-over accumulates
// into a visible failure.
TEST(MmsgBatchLive, SustainedBidirectionalTrafficStaysIntact)
{
	Peer server;
	RakSleep(2);
	Peer client;

	unsigned short serverPort = 0;
	std::string reason;
	ASSERT_TRUE(ConnectLocally(server, client, 1, &serverPort, &reason)) << reason;

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);
	const SystemAddress clientAddress = server->GetSystemAddressFromIndex(0);
	ASSERT_NE(clientAddress, UNASSIGNED_SYSTEM_ADDRESS) << "server has no connected client";

	const unsigned ROUNDS = 30;
	const unsigned PER_ROUND = MMSG_BATCH_MAX + 16; // straddles the flush boundary
	const unsigned TOTAL = ROUNDS * PER_ROUND;

	unsigned clientToServer = 0, serverToClient = 0;
	uint32_t nextFromClient = 0, nextFromServer = 0;
	std::vector<char> message;

	for (unsigned round = 0; round < ROUNDS; ++round)
	{
		// Both ends queue a burst that crosses the batch boundary, then both are
		// drained. Repeat: fill, flush, refill.
		for (unsigned i = 0; i < PER_ROUND; ++i)
		{
			const uint32_t sequence = round * PER_ROUND + i;
			BuildMessage(message, sequence);
			ASSERT_NE(client->Send(message.data(), PAYLOAD_BYTES, MafiaNet::Priority::High,
			                       MafiaNet::Reliability::ReliableOrdered, 0, serverAddress, false),
			          0u);
			ASSERT_NE(server->Send(message.data(), PAYLOAD_BYTES, MafiaNet::Priority::High,
			                       MafiaNet::Reliability::ReliableOrdered, 0, clientAddress, false),
			          0u);
		}

		const TimeMS roundDeadline = GetTimeMS() + 30000;
		const unsigned wantEach = (round + 1) * PER_ROUND;
		while (GetTimeMS() < roundDeadline &&
		       (clientToServer < wantEach || serverToClient < wantEach))
		{
			for (auto pkt : server.incoming())
			{
				if (pkt->data[0] != ID_BATCH_TEST) continue;
				uint32_t sequence = 0;
				std::string why;
				ASSERT_TRUE(VerifyMessage(pkt->data, pkt->length, &sequence, &why)) << why;
				ASSERT_EQ(sequence, nextFromClient++) << "client->server stream broke";
				++clientToServer;
			}
			for (auto pkt : client.incoming())
			{
				if (pkt->data[0] != ID_BATCH_TEST) continue;
				uint32_t sequence = 0;
				std::string why;
				ASSERT_TRUE(VerifyMessage(pkt->data, pkt->length, &sequence, &why)) << why;
				ASSERT_EQ(sequence, nextFromServer++) << "server->client stream broke";
				++serverToClient;
			}
			RakSleep(5);
		}

		ASSERT_EQ(clientToServer, wantEach) << "client->server stalled in round " << round;
		ASSERT_EQ(serverToClient, wantEach) << "server->client stalled in round " << round;
	}

	EXPECT_EQ(clientToServer, TOTAL);
	EXPECT_EQ(serverToClient, TOTAL);
}
