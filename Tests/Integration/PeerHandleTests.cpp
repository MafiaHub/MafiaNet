/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/DS_List.h"
#include "mafianet/peerinterface.h"
#include "mafianet/PeerHandle.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"

#include <utility> // std::move

using namespace MafiaNet;

/// Verifies the RAII handles Peer / PacketPtr (issue #16):
/// - Peer is movable and non-copyable; a moved-from handle is empty (no
///   double-free at scope exit).
/// - PacketPtr::id() returns 255 for a null/empty packet, the first byte for a
///   plain packet, and the post-timestamp byte for an ID_TIMESTAMP packet.
/// - Peer::receive() yields an empty PacketPtr when nothing is queued, and the
///   move-assignment in a receive loop frees each real packet without leaking.

// A distinct port so a full-suite run does not collide with other tests.
static const unsigned short PEERHANDLE_TEST_PORT = 61015;

// A user message id carried behind an ID_TIMESTAMP prefix, used to exercise the
// timestamp-aware branch of PacketPtr::id().
static const unsigned char PEERHANDLE_TS_ID = (unsigned char) (ID_USER_PACKET_ENUM + 7);

// --- Peer: movable, non-copyable, moved-from is empty ---
TEST(PeerHandle, MoveSemantics)
{
	Peer a;
	ASSERT_TRUE(a && a.get() != nullptr) << "default-constructed Peer is not valid";
	RakPeerInterface *raw = a.get();

	Peer b(std::move(a));
	ASSERT_TRUE(!a && a.get() == nullptr) << "moved-from Peer (ctor) is not empty";
	ASSERT_TRUE(b && b.get() == raw) << "move ctor did not transfer the instance";

	Peer c;
	c = std::move(b);
	ASSERT_TRUE(!b && b.get() == nullptr) << "moved-from Peer (assign) is not empty";
	ASSERT_TRUE(c && c.get() == raw) << "move assign did not transfer the instance";
	// a and b destruct empty (no-op); c destructs the single live instance.
}

// --- PacketPtr: null/empty path and empty receive() ---
TEST(PeerHandle, NullPacketPtrAndEmptyReceive)
{
	Peer p;
	PacketPtr nullPkt(p.get(), nullptr);
	ASSERT_FALSE(nullPkt) << "null PacketPtr is unexpectedly truthy";
	ASSERT_EQ(nullPkt.id(), 255) << "null PacketPtr id() is not 255";

	// A peer that was never started has nothing queued.
	PacketPtr none = p.receive();
	ASSERT_FALSE(none) << "receive() on an un-started peer is not empty";
}

// --- Real packets over loopback: plain and timestamp id() branches, then
// incoming(): the range adaptor drains identically to the manual loop ---
TEST(PeerHandle, LoopbackIdBranchesAndIncomingRange)
{
	Peer server;
	Peer client;

	SocketDescriptor serverSd(PEERHANDLE_TEST_PORT, 0);
	ASSERT_EQ(server->Startup(1, &serverSd, 1), RAKNET_STARTED)
		<< "server Startup failed (port " << PEERHANDLE_TEST_PORT << " in use?)";
	server->SetMaximumIncomingConnections(1);

	SocketDescriptor clientSd;
	ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED) << "client Startup failed";

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(PEERHANDLE_TEST_PORT);

	ASSERT_EQ(client->Connect("127.0.0.1", PEERHANDLE_TEST_PORT, 0, 0), CONNECTION_ATTEMPT_STARTED)
		<< "Connect did not start";

	// Wait for ID_CONNECTION_REQUEST_ACCEPTED on the client. This identifier is
	// NOT timestamp-prefixed, so it exercises id()'s plain (first-byte) branch.
	bool connected = false;
	bool plainBranchOk = false;
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !connected)
	{
		for (PacketPtr pkt = server.receive(); pkt; pkt = server.receive())
		{
			// Drain the server side (ID_NEW_INCOMING_CONNECTION, etc.).
		}
		for (PacketPtr pkt = client.receive(); pkt; pkt = client.receive())
		{
			if (pkt.id() == ID_CONNECTION_REQUEST_ACCEPTED)
			{
				ASSERT_FALSE(pkt->data[0] == ID_TIMESTAMP || pkt.id() != (unsigned char) pkt->data[0])
					<< "plain-branch id() did not match data[0]";
				plainBranchOk = true;
				connected = true;
			}
		}
		RakSleep(30);
	}
	ASSERT_TRUE(connected) << "client did not connect within 10s";
	ASSERT_TRUE(plainBranchOk) << "Did not observe ID_CONNECTION_REQUEST_ACCEPTED";

	// Send a timestamp-prefixed user message and verify id() reads the byte that
	// follows the MessageID + Time prefix on the server side.
	BitStream bs;
	bs.Write((MessageID) ID_TIMESTAMP);
	bs.Write(GetTime());
	bs.Write((MessageID) PEERHANDLE_TS_ID);
	client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false);

	bool timestampBranchOk = false;
	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && !timestampBranchOk)
	{
		for (PacketPtr pkt = server.receive(); pkt; pkt = server.receive())
		{
			if (pkt->data[0] == ID_TIMESTAMP)
			{
				ASSERT_EQ(pkt.id(), PEERHANDLE_TS_ID)
					<< "timestamp-branch id() returned the wrong byte";
				timestampBranchOk = true;
			}
		}
		for (PacketPtr pkt = client.receive(); pkt; pkt = client.receive())
		{
			// Drain the client side.
		}
		RakSleep(30);
	}
	ASSERT_TRUE(timestampBranchOk) << "server did not receive the timestamp message within 5s";

	// --- incoming(): the range adaptor drains identically to the manual loop ---
	// Send a batch of timestamp-prefixed messages, then consume them with the
	// range-based form. Each iteration yields a fresh, auto-deallocated PacketPtr;
	// draining a second time must leave the queue empty (no leaks under ASan).
	const int kBatch = 5;
	for (int i = 0; i < kBatch; ++i)
	{
		BitStream batchBs;
		batchBs.Write((MessageID) ID_TIMESTAMP);
		batchBs.Write(GetTime());
		batchBs.Write((MessageID) PEERHANDLE_TS_ID);
		client->Send(&batchBs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false);
	}

	int rangeReceived = 0;
	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && rangeReceived < kBatch)
	{
		for (auto pkt : server.incoming())
		{
			if (pkt.id() == PEERHANDLE_TS_ID)
				++rangeReceived;
		}
		// Keep the client side drained via the range form as well.
		for (auto pkt : client.incoming())
		{
			(void) pkt;
		}
		RakSleep(30);
	}
	ASSERT_EQ(rangeReceived, kBatch)
		<< "incoming() drained " << rangeReceived << " of " << kBatch << " messages";

	// The queue is now empty: a fresh range must iterate zero times.
	int leftover = 0;
	for (auto pkt : server.incoming())
	{
		(void) pkt;
		++leftover;
	}
	ASSERT_EQ(leftover, 0) << "incoming() yielded " << leftover << " packets from a drained queue";

	// server and client are RAII Peers: they destruct (DestroyInstance) here,
	// including on every early ASSERT return above — that is the point of the wrappers.
}
