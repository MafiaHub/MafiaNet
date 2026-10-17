/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Nine clients on the SimulatedNetwork connect to a server, then silently
 *  drop, gracefully disconnect and reconnect at random. The server must detect
 *  every silent drop through its timeout, and no client may ever hold more
 *  than one connection.
 */

#include <gtest/gtest.h>

#include <map>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
	const int kClients = 9;
	const TimeMS kServerTimeoutMs = 2000;
	const TimeMS kClientTimeoutMs = 5000;
	// A silent drop is noticed once a reliable message (at worst the keepalive
	// ping, sent after timeout/2 without reliable traffic) goes unacknowledged
	// for the timeout. Leave one more timeout/2 of slack for the update cadence.
	const uint32_t kDetectionBudgetMs = kServerTimeoutMs + kServerTimeoutMs / 2;

	class DroppedConnectionConvertSim : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			server = net.AddPeer(kClients, kClients);
			server->SetTimeoutTime(kServerTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
			serverAddress = net.AddressOf(server);
			net.SetTap([this](const SystemAddress &from, const SystemAddress &to, const char *, int) {
				if (to == serverAddress)
					lastHeardFrom[from] = net.Now();
			});
			for (int i = 0; i < kClients; ++i)
			{
				clients[i] = net.AddPeer(1);
				clients[i]->SetTimeoutTime(kClientTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
				ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(clients[i], server)) << "client " << i;
				net.Step(1000);
			}
		}

		static unsigned short ConnectionCount(RakPeer *peer)
		{
			unsigned short count = 0;
			peer->GetConnectionList(nullptr, &count);
			return count;
		}

		// ASSERTs no client has more than one connection; returns how many have one.
		int ConnectedClients()
		{
			int connected = 0;
			for (int i = 0; i < kClients; ++i)
			{
				const unsigned short count = ConnectionCount(clients[i]);
				EXPECT_LE(count, 1) << "client " << i << " has " << count << " connections";
				if (count == 1)
					++connected;
			}
			return connected;
		}

		bool BusyOrConnected(int i)
		{
			const ConnectionState state = clients[i]->GetConnectionState(serverAddress);
			return state == IS_CONNECTED || state == IS_CONNECTING || state == IS_PENDING || state == IS_DISCONNECTING;
		}

		uint32_t NextRandom()
		{
			lcg = lcg * 1664525u + 1013904223u;
			return lcg >> 8;
		}

		SimulatedNetwork net;
		RakPeer *server = nullptr;
		RakPeer *clients[kClients] = {};
		SystemAddress serverAddress;
		std::map<SystemAddress, TimeUS> lastHeardFrom;
		uint32_t lcg = 12345;
	};
}

TEST_F(DroppedConnectionConvertSim, AllClientsConnect)
{
	EXPECT_EQ(kClients, ConnectedClients());
	EXPECT_EQ(kClients, ConnectionCount(server));
}

TEST_F(DroppedConnectionConvertSim, SilentDropIsDetectedByTheServerTimeout)
{
	clients[0]->CloseConnection(serverAddress, false, 0);
	ASSERT_TRUE(net.RunUntil([&] { return ConnectionCount(clients[0]) == 0; }, 100));
	EXPECT_EQ(kClients, ConnectionCount(server)) << "a silent close must not tell the server";

	ASSERT_TRUE(net.RunUntilReceived(server, ID_CONNECTION_LOST, kDetectionBudgetMs));
	// The timeout runs from the last datagram the server got from the client.
	const TimeUS silentForMs = (net.Now() - lastHeardFrom[net.AddressOf(clients[0])]) / 1000;
	EXPECT_GE(silentForMs, kServerTimeoutMs) << "dropped before the timeout elapsed";
	EXPECT_LE(silentForMs, kDetectionBudgetMs);
	EXPECT_EQ(clients[0]->GetMyGUID(), net.Find(server, ID_CONNECTION_LOST)->guid);
	EXPECT_EQ(kClients - 1, ConnectionCount(server));
	EXPECT_FALSE(net.Received(server, ID_DISCONNECTION_NOTIFICATION));
}

TEST_F(DroppedConnectionConvertSim, RandomDropsAndReconnectsStayConsistent)
{
	const TimeUS start = net.Now();
	int timeoutChecks = 0;
	while (net.Now() - start < (TimeUS) 30000 * 1000)
	{
		switch (NextRandom() % 4)
		{
		case 0: // silently drop one client
			clients[NextRandom() % kClients]->CloseConnection(serverAddress, false, 0);
			break;
		case 1: // reconnect one client unless it is busy
		{
			const int index = (int) (NextRandom() % kClients);
			if (!BusyOrConnected(index))
				ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(clients[index], server)) << "client " << index;
			break;
		}
		case 2: // every client: drop (silently or not) or reconnect
			for (int index = 0; index < kClients; ++index)
			{
				if (NextRandom() % 2 == 0)
					clients[index]->CloseConnection(serverAddress, NextRandom() % 2 == 0, 0);
				else if (!BusyOrConnected(index))
					ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(clients[index], server)) << "client " << index;
			}
			break;
		case 3: // quiet period: the server must catch up with every drop
		{
			++timeoutChecks;
			const bool consistent = net.RunUntil([&] { return ConnectedClients() == ConnectionCount(server); }, kDetectionBudgetMs);
			ASSERT_TRUE(consistent) << "timeout on dropped clients not detected: server has " << ConnectionCount(server)
				<< " connections, " << ConnectedClients() << " clients are connected";
			break;
		}
		}
		ConnectedClients();
		ASSERT_FALSE(HasFailure());
		net.Step(10);
	}
	EXPECT_GT(timeoutChecks, 5);

	// Every client can get back in once the dust settles. A reconnect that
	// races the server still holding the silently dropped connection is
	// refused (ID_ALREADY_CONNECTED) until the timeout clears it, so retry.
	const bool allBack = net.RunUntil([&] {
		for (int index = 0; index < kClients; ++index)
			if (!BusyOrConnected(index))
				EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(clients[index], server)) << "client " << index;
		return ConnectedClients() == kClients && ConnectionCount(server) == kClients;
	}, 20000, 100);
	EXPECT_TRUE(allBack) << ConnectedClients() << " clients and " << ConnectionCount(server) << " server connections";
}
