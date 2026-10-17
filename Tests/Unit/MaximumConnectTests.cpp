/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Eight peers on the SimulatedNetwork all try to connect to each other while
 *  each allows only four connections: every extra connection must be refused.
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
	const int kPeerNum = 8;
	const unsigned short kMaxConnections = 4;

	int ConnectionCount(RakPeer *peer)
	{
		DataStructures::List<SystemAddress> systemList;
		DataStructures::List<RakNetGUID> guidList;
		peer->GetSystemList(systemList, guidList);
		return (int) guidList.Size();
	}
}

TEST(MaximumConnectSim, GetMaximumIncomingConnectionsReturnsTheSetValue)
{
	SimulatedNetwork net;
	RakPeer *peer = net.AddPeer(kMaxConnections, kMaxConnections);
	EXPECT_EQ(kMaxConnections, peer->GetMaximumIncomingConnections());

	peer->SetMaximumIncomingConnections(2);
	EXPECT_EQ(2u, peer->GetMaximumIncomingConnections());
}

TEST(MaximumConnectSim, MeshNeverExceedsTheConnectionLimit)
{
	SimulatedNetwork net;
	RakPeer *peers[kPeerNum];
	for (int i = 0; i < kPeerNum; ++i)
		peers[i] = net.AddPeer(kMaxConnections, kMaxConnections);

	for (int i = 0; i < kPeerNum; ++i)
		for (int j = i + 1; j < kPeerNum; ++j)
			ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(peers[i], peers[j])) << i << " -> " << j;

	// The limit must hold at every instant, not only once things settle.
	const auto allAttemptsResolved = [&] {
		for (int i = 0; i < kPeerNum; ++i)
			for (int j = 0; j < kPeerNum; ++j)
			{
				if (i == j)
					continue;
				const ConnectionState state = peers[i]->GetConnectionState(net.AddressOf(peers[j]));
				if (state == IS_PENDING || state == IS_CONNECTING || state == IS_DISCONNECTING ||
					state == IS_SILENTLY_DISCONNECTING)
					return false;
			}
		return true;
	};
	int worst = 0;
	const bool resolved = net.RunUntil([&] {
		for (int i = 0; i < kPeerNum; ++i)
			worst = std::max(worst, ConnectionCount(peers[i]));
		return allAttemptsResolved();
	}, 20000);
	EXPECT_LE(worst, (int) kMaxConnections) << "an extra connection was allowed at some point";
	ASSERT_TRUE(resolved) << "connection attempts still pending after 20s";

	int total = 0;
	for (int i = 0; i < kPeerNum; ++i)
	{
		const int connections = ConnectionCount(peers[i]);
		EXPECT_LE(connections, (int) kMaxConnections) << "peer " << i << " has " << connections << " connections";
		total += connections;

		// Both ends agree on every link.
		for (int j = 0; j < kPeerNum; ++j)
			if (i != j)
				EXPECT_EQ(peers[i]->GetConnectionState(net.AddressOf(peers[j])) == IS_CONNECTED,
					peers[j]->GetConnectionState(net.AddressOf(peers[i])) == IS_CONNECTED)
					<< "asymmetric link " << i << " <-> " << j;
	}
	// A full mesh would need 7 per peer: some attempts were refused, but the
	// peers did connect rather than refusing everything. (Not every peer
	// reaches the limit: half-open incoming handshakes from peers that then
	// ran out of slots hold a target's slots until they time out, and its own
	// attempts fail meanwhile. Nobody retries in this scenario.)
	EXPECT_LT(total, kPeerNum * (kPeerNum - 1));
	EXPECT_GT(total, 0);
}
