/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Regression coverage for https://github.com/MafiaHub/MafiaNet/issues/7:
// destroying a RakPeer while its connections and internal threads are live
// must not leave a leaked network/recv thread touching freed memory. Each
// scenario below churns the Startup/Connect/Shutdown/DestroyInstance cycle
// that exposed the teardown race; correctness is asserted deterministically,
// and the memory-safety aspect is what ASan/TSan CI runs of this suite verify.

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"

#include <vector>

using namespace MafiaNet;

class PeerTeardown : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Destroyed clients disappear silently (no disconnect notification), so
		// their server-side slots linger as zombies until the server's timeout
		// reaps them. Give the server enough headroom that every churn round can
		// connect fresh clients while earlier rounds' zombies are still pending.
		// TrackNewClient() hands out references into this vector; reserve enough
		// that no test can trigger a reallocation and invalidate them.
		clients.reserve(kServerCapacity);

		server = RakPeerInterface::GetInstance();
		SocketDescriptor sd(0, "127.0.0.1");
		ASSERT_EQ(server->Startup(kServerCapacity, &sd, 1), RAKNET_STARTED);
		server->SetMaximumIncomingConnections(kServerCapacity);
		serverPort = server->GetInternalID().GetPort();
	}

	// Every client is registered in `clients`, so cleanup survives a failed
	// fatal assertion mid-test: any peer not already destroyed by the test body
	// is shut down here.
	void TearDown() override
	{
		for (RakPeerInterface *&client : clients)
		{
			if (client)
			{
				client->Shutdown(100);
				RakPeerInterface::DestroyInstance(client);
				client = nullptr;
			}
		}
		if (server)
		{
			server->Shutdown(100);
			RakPeerInterface::DestroyInstance(server);
		}
	}

	// Create a client peer that TearDown() will clean up if the test body
	// doesn't destroy it first. Returns a reference to the tracked slot so the
	// test can mark it destroyed (slot = nullptr) after DestroyInstance.
	RakPeerInterface *&TrackNewClient()
	{
		clients.push_back(RakPeerInterface::GetInstance());
		return clients.back();
	}

	static void DestroyTrackedClient(RakPeerInterface *&slot)
	{
		RakPeerInterface::DestroyInstance(slot);
		slot = nullptr;
	}

	// Pump a peer's receive queue so its user-thread bookkeeping advances.
	static void Pump(RakPeerInterface *peer)
	{
		Packet *packet;
		while ((packet = peer->Receive()) != nullptr)
			peer->DeallocatePacket(packet);
	}

	// Wait until BOTH sides have observed the connection: the client reports
	// IS_CONNECTED to the server address, and the server reports IS_CONNECTED
	// for the client's bound address.
	bool WaitForConnection(RakPeerInterface *client, TimeMS timeoutMs)
	{
		SystemAddress serverAddress("127.0.0.1", serverPort);
		SystemAddress clientAddress("127.0.0.1", client->GetInternalID().GetPort());
		TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < timeoutMs)
		{
			Pump(client);
			Pump(server);
			if (client->GetConnectionState(serverAddress) == IS_CONNECTED &&
				server->GetConnectionState(clientAddress) == IS_CONNECTED)
				return true;
			RakSleep(10);
		}
		return false;
	}

	static const int kMaxClients = 8;
	static const int kServerCapacity = 64;
	RakPeerInterface *server = nullptr;
	unsigned short serverPort = 0;
	std::vector<RakPeerInterface *> clients;
};

// A single client repeatedly connects and is torn down mid-connection. Every
// cycle must start up successfully (a leaked recv thread from the previous
// cycle would hold the port / corrupt the allocator) and reconnect.
TEST_F(PeerTeardown, DestroyWithLiveConnectionThenRecreateRepeatedly)
{
	const int kCycles = 12;
	for (int cycle = 0; cycle < kCycles; cycle++)
	{
		RakPeerInterface *&client = TrackNewClient();
		SocketDescriptor clientSd(0, "127.0.0.1");
		ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED)
			<< "cycle " << cycle << ": client failed to start";

		ASSERT_EQ(client->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED)
			<< "cycle " << cycle;
		ASSERT_TRUE(WaitForConnection(client, 5000))
			<< "cycle " << cycle << ": connection not observed on both sides";

		// Tear the peer down while the connection is fully live. Alternate
		// between a graceful window and an immediate teardown so both paths
		// (flush-and-close and drop-everything) run under sanitizers.
		client->Shutdown(cycle % 2 == 0 ? 100 : 0);
		EXPECT_FALSE(client->IsActive()) << "cycle " << cycle;
		DestroyTrackedClient(client);
	}

	// The server must survive all of that with its own threads intact: it can
	// still accept a fresh connection afterwards.
	RakPeerInterface *&client = TrackNewClient();
	SocketDescriptor clientSd(0, "127.0.0.1");
	ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);
	ASSERT_EQ(client->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	EXPECT_TRUE(WaitForConnection(client, 5000)) << "server no longer accepts connections after churn";
	client->Shutdown(100);
	DestroyTrackedClient(client);
}

// Several clients are destroyed at once while all their connections are live,
// then immediately recreated -- the pattern from
// ManyClientsOneServerDeallocateBlockingTests that exposed the race.
TEST_F(PeerTeardown, DestroyManyClientsSimultaneouslyWhileConnected)
{
	for (int round = 0; round < 3; round++)
	{
		// Indices into the fixture-tracked list for this round's clients.
		std::vector<size_t> roundClients;
		for (int i = 0; i < kMaxClients; i++)
		{
			RakPeerInterface *&client = TrackNewClient();
			roundClients.push_back(clients.size() - 1);
			SocketDescriptor sd(0, "127.0.0.1");
			ASSERT_EQ(client->Startup(1, &sd, 1), RAKNET_STARTED) << "round " << round << " client " << i;
			ASSERT_EQ(client->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
		}
		for (int i = 0; i < kMaxClients; i++)
			ASSERT_TRUE(WaitForConnection(clients[roundClients[i]], 5000)) << "round " << round << " client " << i;

		// No Shutdown() call first: DestroyInstance itself must cope with live
		// connections and running threads.
		for (size_t index : roundClients)
			DestroyTrackedClient(clients[index]);
	}
}
