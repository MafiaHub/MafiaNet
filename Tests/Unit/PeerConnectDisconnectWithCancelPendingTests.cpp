/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Eight peers on the SimulatedNetwork repeatedly disconnect, cancel any
 *  pending connection attempt and connect again; at the end the full mesh
 *  must come back. Also checks CancelConnectionAttempt on its own.
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

class PeerConnectDisconnectWithCancelPendingSim : public ::testing::Test
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

	void CancelAllAttempts()
	{
		for (int i = 0; i < kPeerNum; ++i)
			for (int j = i + 1; j < kPeerNum; ++j)
				peers[i]->CancelConnectionAttempt(net.AddressOf(peers[j]));
	}

	SimulatedNetwork net;
	RakPeer *peers[kPeerNum] = {};
};
}

TEST_F(PeerConnectDisconnectWithCancelPendingSim, MeshReconnectsWithCancelledPendingAttempts)
{
	for (int i = 0; i < kPeerNum; ++i)
		for (int j = i + 1; j < kPeerNum; ++j)
			ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(peers[i], peers[j])) << i << " -> " << j;

	for (int cycle = 0; cycle < kCycles; ++cycle)
	{
		CloseAllConnections();
		net.Step(100);
		CancelAllAttempts();
		net.Step(100);

		for (int i = 0; i < kPeerNum; ++i)
			for (int j = i + 1; j < kPeerNum; ++j)
			{
				if (net.StartConnect(peers[i], peers[j]) == CONNECTION_ATTEMPT_STARTED)
					continue;
				// Refused: the cancel must have dropped any pending attempt, so
				// the only acceptable reason is an existing connection.
				const ConnectionState state = StateOf(i, j);
				ASSERT_FALSE(state == IS_CONNECTING || state == IS_PENDING)
				    << "cycle " << cycle << ": pending attempt " << i << " -> " << j << " was not cancelled";
				ASSERT_TRUE(state == IS_CONNECTED || state == IS_DISCONNECTING)
				    << "cycle " << cycle << ": Connect " << i << " -> " << j << " refused in state " << state;
			}
	}

	// Let the last disconnects and handshakes play out.
	net.Step(2000);

	for (int i = 0; i < kPeerNum; ++i)
		for (int j = i + 1; j < kPeerNum; ++j)
		{
			peers[i]->CancelConnectionAttempt(net.AddressOf(peers[j]));
			if (net.StartConnect(peers[i], peers[j]) != CONNECTION_ATTEMPT_STARTED)
			{
				ASSERT_NE(0, ConnectionCount(i)) << "Connect " << i << " -> " << j << " refused with no connection at all";
			}
		}

	net.RunUntil([&]
	             {
		for (int i = 0; i < kPeerNum; ++i)
			if (ConnectionCount(i) != kPeerNum - 1)
				return false;
		return true; }, 5000);
	for (int i = 0; i < kPeerNum; ++i)
		EXPECT_EQ(kPeerNum - 1, ConnectionCount(i)) << "peer " << i << " did not reconnect to all peers";

	// The churn really happened, and both ends saw each handshake. A link
	// still handshaking when a cycle closes connections survives it, so every
	// other cycle rebuilds the mesh.
	const int kPairs = kPeerNum * (kPeerNum - 1) / 2;
	int incoming = 0, accepted = 0;
	for (int i = 0; i < kPeerNum; ++i)
		for (const auto &message : net.Inbox(peers[i]))
		{
			incoming += message.id == ID_NEW_INCOMING_CONNECTION;
			accepted += message.id == ID_CONNECTION_REQUEST_ACCEPTED;
		}
	EXPECT_EQ(incoming, accepted);
	EXPECT_GE(accepted, kPairs * kCycles / 2);
}

TEST(CancelConnectionAttemptSim, CancelledAttemptNeverConnects)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));
	EXPECT_EQ(IS_PENDING, client->GetConnectionState(serverAddress));
	client->CancelConnectionAttempt(serverAddress);
	EXPECT_EQ(IS_NOT_CONNECTED, client->GetConnectionState(serverAddress));

	net.Step(2000);
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_FALSE(net.Received(server, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(IS_NOT_CONNECTED, client->GetConnectionState(serverAddress));

	// The slot is free again: a fresh attempt goes through.
	ASSERT_TRUE(net.Connect(client, server));
}
