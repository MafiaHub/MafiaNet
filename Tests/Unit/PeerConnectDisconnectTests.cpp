/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Eight peers on the SimulatedNetwork connect to each other, all disconnect,
 *  connect again, over and over; at the end the full mesh must come back.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
	const int kPeerNum = 8;
	const unsigned short kMaxConnections = kPeerNum * 3; // generous, to rule out the limit
	const int kCycles = 25;

	class PeerConnectDisconnectSim : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			for (int i = 0; i < kPeerNum; ++i)
				peers[i] = net.AddPeer(kMaxConnections, kMaxConnections);
		}

		ConnectionState StateOf(int i, int j)
		{
			return peers[i]->GetConnectionState(net.AddressOf(peers[j]));
		}

		// Connected, or something is already under way for this link.
		bool BusyOrConnected(int i, int j)
		{
			const ConnectionState state = StateOf(i, j);
			return state == IS_CONNECTED || state == IS_CONNECTING || state == IS_PENDING || state == IS_DISCONNECTING;
		}

		int ConnectionCount(int i)
		{
			DataStructures::List<SystemAddress> systemList;
			DataStructures::List<RakNetGUID> guidList;
			peers[i]->GetSystemList(systemList, guidList);
			return (int) guidList.Size();
		}

		void CloseAllConnections()
		{
			for (int i = 0; i < kPeerNum; ++i)
			{
				DataStructures::List<SystemAddress> systemList;
				DataStructures::List<RakNetGUID> guidList;
				peers[i]->GetSystemList(systemList, guidList);
				for (unsigned j = 0; j < systemList.Size(); ++j)
					peers[i]->CloseConnection(systemList[j], true, 0, Priority::Low);
			}
		}

		// No outgoing attempt (i -> j, j > i) is still connecting or pending.
		bool ConnectionRequestsComplete()
		{
			for (int i = 0; i < kPeerNum; ++i)
				for (int j = i + 1; j < kPeerNum; ++j)
				{
					const ConnectionState state = StateOf(i, j);
					if (state == IS_CONNECTING || state == IS_PENDING)
						return false;
				}
			return true;
		}

		SimulatedNetwork net;
		RakPeer *peers[kPeerNum] = {};
	};
}

TEST_F(PeerConnectDisconnectSim, MeshReconnectsAfterRepeatedDisconnects)
{
	for (int i = 0; i < kPeerNum; ++i)
		for (int j = i + 1; j < kPeerNum; ++j)
			ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(peers[i], peers[j])) << i << " -> " << j;

	for (int cycle = 0; cycle < kCycles; ++cycle)
	{
		CloseAllConnections();
		net.Step(100);

		for (int i = 0; i < kPeerNum; ++i)
			for (int j = i + 1; j < kPeerNum; ++j)
				if (!BusyOrConnected(i, j))
				{
					ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(peers[i], peers[j]))
						<< "cycle " << cycle << ": " << i << " -> " << j << " in state " << StateOf(i, j);
				}

		ASSERT_TRUE(net.RunUntil([&] { return ConnectionRequestsComplete(); }, 10000))
			<< "cycle " << cycle << ": connection requests never completed";
	}

	// Bring the full mesh back up, re-initiating any link that dropped out.
	bool allConnected = false;
	int failedPeer = -1;
	int failedPeerConns = 0;
	const TimeUS convergeStart = net.Now();
	while (net.Now() - convergeStart < (TimeUS) 30000 * 1000)
	{
		for (int i = 0; i < kPeerNum; ++i)
			for (int j = i + 1; j < kPeerNum; ++j)
				if (!BusyOrConnected(i, j))
				{
					// STARTED is success; the other two are benign races with
					// a link that came up in between.
					const ConnectionAttemptResult result = net.StartConnect(peers[i], peers[j]);
					ASSERT_TRUE(result == CONNECTION_ATTEMPT_STARTED || result == ALREADY_CONNECTED_TO_ENDPOINT ||
						result == CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS)
						<< i << " -> " << j << " failed with result " << (int) result;
				}

		ASSERT_TRUE(net.RunUntil([&] { return ConnectionRequestsComplete(); }, 10000));

		allConnected = true;
		for (int i = 0; i < kPeerNum && allConnected; ++i)
			if (ConnectionCount(i) != kPeerNum - 1)
			{
				allConnected = false;
				failedPeer = i;
				failedPeerConns = ConnectionCount(i);
			}
		if (allConnected)
			break;
		net.Step(100);
	}

	ASSERT_TRUE(allConnected) << "not all peers reconnected: peer " << failedPeer << " has " << failedPeerConns << " connections";

	// The churn really happened (every cycle tore down and rebuilt every
	// link), and both ends saw each handshake.
	const int kPairs = kPeerNum * (kPeerNum - 1) / 2;
	int incoming = 0, accepted = 0;
	for (int i = 0; i < kPeerNum; ++i)
		for (const auto &message : net.Inbox(peers[i]))
		{
			incoming += message.id == ID_NEW_INCOMING_CONNECTION;
			accepted += message.id == ID_CONNECTION_REQUEST_ACCEPTED;
		}
	EXPECT_EQ(incoming, accepted);
	EXPECT_GE(accepted, kPairs * kCycles);
}
