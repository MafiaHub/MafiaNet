/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "PeerHandleTest.h"

#include <utility> // std::move

// A distinct port so a full-suite run does not collide with other tests.
static const unsigned short PEERHANDLE_TEST_PORT = 61015;

// A user message id carried behind an ID_TIMESTAMP prefix, used to exercise the
// timestamp-aware branch of PacketPtr::id().
static const unsigned char PEERHANDLE_TS_ID = (unsigned char) (ID_USER_PACKET_ENUM + 7);

int PeerHandleTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	// --- 1. Peer: movable, non-copyable, moved-from is empty ---
	{
		Peer a;
		if (!a || a.get() == nullptr)
		{
			if (isVerbose) printf("default-constructed Peer is not valid\n");
			return 1;
		}
		RakPeerInterface *raw = a.get();

		Peer b(std::move(a));
		if (a || a.get() != nullptr)
		{
			if (isVerbose) printf("moved-from Peer (ctor) is not empty\n");
			return 2;
		}
		if (!b || b.get() != raw)
		{
			if (isVerbose) printf("move ctor did not transfer the instance\n");
			return 3;
		}

		Peer c;
		c = std::move(b);
		if (b || b.get() != nullptr)
		{
			if (isVerbose) printf("moved-from Peer (assign) is not empty\n");
			return 4;
		}
		if (!c || c.get() != raw)
		{
			if (isVerbose) printf("move assign did not transfer the instance\n");
			return 5;
		}
		// a and b destruct empty (no-op); c destructs the single live instance.
	}

	// --- 2. PacketPtr: null/empty path and empty receive() ---
	{
		Peer p;
		PacketPtr nullPkt(p.get(), nullptr);
		if (nullPkt)
		{
			if (isVerbose) printf("null PacketPtr is unexpectedly truthy\n");
			return 10;
		}
		if (nullPkt.id() != 255)
		{
			if (isVerbose) printf("null PacketPtr id() is not 255\n");
			return 11;
		}

		// A peer that was never started has nothing queued.
		PacketPtr none = p.receive();
		if (none)
		{
			if (isVerbose) printf("receive() on an un-started peer is not empty\n");
			return 12;
		}
	}

	// --- 3. Real packets over loopback: plain and timestamp id() branches ---
	Peer server;
	Peer client;

	SocketDescriptor serverSd(PEERHANDLE_TEST_PORT, 0);
	if (server->Startup(1, &serverSd, 1) != RAKNET_STARTED)
	{
		if (isVerbose) printf("server Startup failed (port %u in use?)\n", PEERHANDLE_TEST_PORT);
		return 20;
	}
	server->SetMaximumIncomingConnections(1);

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{
		if (isVerbose) printf("client Startup failed\n");
		return 21;
	}

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(PEERHANDLE_TEST_PORT);

	if (client->Connect("127.0.0.1", PEERHANDLE_TEST_PORT, 0, 0) != CONNECTION_ATTEMPT_STARTED)
	{
		if (isVerbose) printf("Connect did not start\n");
		return 22;
	}

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
				if (pkt->data[0] == ID_TIMESTAMP || pkt.id() != (unsigned char) pkt->data[0])
				{
					if (isVerbose) printf("plain-branch id() did not match data[0]\n");
					return 23;
				}
				plainBranchOk = true;
				connected = true;
			}
		}
		RakSleep(30);
	}
	if (!connected)
	{
		if (isVerbose) printf("client did not connect within 10s\n");
		return 24;
	}
	if (!plainBranchOk)
		return 25;

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
				if (pkt.id() != PEERHANDLE_TS_ID)
				{
					if (isVerbose) printf("timestamp-branch id() returned the wrong byte\n");
					return 30;
				}
				timestampBranchOk = true;
			}
		}
		for (PacketPtr pkt = client.receive(); pkt; pkt = client.receive())
		{
			// Drain the client side.
		}
		RakSleep(30);
	}
	if (!timestampBranchOk)
	{
		if (isVerbose) printf("server did not receive the timestamp message within 5s\n");
		return 31;
	}

	// --- 4. incoming(): the range adaptor drains identically to the manual loop ---
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
	if (rangeReceived != kBatch)
	{
		if (isVerbose) printf("incoming() drained %d of %d messages\n", rangeReceived, kBatch);
		return 40;
	}

	// The queue is now empty: a fresh range must iterate zero times.
	int leftover = 0;
	for (auto pkt : server.incoming())
	{
		(void) pkt;
		++leftover;
	}
	if (leftover != 0)
	{
		if (isVerbose) printf("incoming() yielded %d packets from a drained queue\n", leftover);
		return 41;
	}

	// server and client are RAII Peers: they destruct (DestroyInstance) here,
	// including on every early return above — that is the point of the wrappers.
	return 0;
}

PeerHandleTest::PeerHandleTest(void)
{
}

PeerHandleTest::~PeerHandleTest(void)
{
}

RakString PeerHandleTest::GetTestName(void)
{
	return "PeerHandleTest";
}

RakString PeerHandleTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "Default-constructed Peer invalid";
	case 2:  return "Moved-from Peer (ctor) not empty";
	case 3:  return "Move ctor did not transfer instance";
	case 4:  return "Moved-from Peer (assign) not empty";
	case 5:  return "Move assign did not transfer instance";
	case 10: return "Null PacketPtr truthy";
	case 11: return "Null PacketPtr id() != 255";
	case 12: return "receive() on un-started peer not empty";
	case 20: return "Server Startup failed";
	case 21: return "Client Startup failed";
	case 22: return "Connect did not start";
	case 23: return "Plain-branch id() mismatch";
	case 24: return "Client did not connect";
	case 25: return "Did not observe ID_CONNECTION_REQUEST_ACCEPTED";
	case 30: return "Timestamp-branch id() mismatch";
	case 31: return "Server did not receive timestamp message";
	case 40: return "incoming() did not drain the batch";
	case 41: return "incoming() yielded packets from a drained queue";
	default: return "Undefined error";
	}
}

void PeerHandleTest::DestroyPeers(void)
{
	// Peers in this test are RAII MafiaNet::Peer locals; they clean themselves
	// up at scope exit, so there is nothing to do here.
}
