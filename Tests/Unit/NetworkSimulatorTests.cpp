/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RakPeerInterface::ApplyNetworkSimulator: outgoing loss and extra latency on
 *  every connection of a peer, in every build configuration (it used to be
 *  compiled into Debug builds only, so a Release game could not reproduce a
 *  bad link).
 *
 *  Runs over the in-process SimulatedNetwork with a perfect link, so every
 *  loss and delay observed here comes from the simulator under test.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
const MessageID kProbe = (MessageID) ID_USER_PACKET_ENUM;

void SendProbe(RakPeer *from, const RakNetGUID &to, Reliability reliability)
{
	BitStream probe;
	probe.Write(kProbe);
	probe.Write((uint32_t) 0xC0FFEE);
	ASSERT_NE(0u, from->Send(&probe, Priority::High, reliability, 0, to, false));
}
}

TEST(NetworkSimulator, ActiveOnlyWhileSomethingIsSet)
{
	SimulatedNetwork net;
	RakPeer *peer = net.AddPeer(1);
	EXPECT_FALSE(peer->IsNetworkSimulatorActive());
	peer->ApplyNetworkSimulator(0.0f, 50, 0);
	EXPECT_TRUE(peer->IsNetworkSimulatorActive());
	peer->ApplyNetworkSimulator(0.0f, 0, 0);
	EXPECT_FALSE(peer->IsNetworkSimulatorActive());
}

TEST(NetworkSimulator, FullLossHoldsReliableDataUntilCleared)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const RakNetGUID serverGuid = server->GetMyGUID();

	client->ApplyNetworkSimulator(1.0f, 0, 0);
	SendProbe(client, serverGuid, Reliability::Reliable);
	EXPECT_FALSE(net.RunUntilReceived(server, kProbe, 1000)) << "a datagram got through 100% simulated loss";

	// The reliability layer still holds the message: it arrives once the link is clean again.
	client->ApplyNetworkSimulator(0.0f, 0, 0);
	EXPECT_TRUE(net.RunUntilReceived(server, kProbe, 5000));
}

TEST(NetworkSimulator, ExtraPingDelaysDelivery)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));

	client->ApplyNetworkSimulator(0.0f, 300, 0);
	SendProbe(client, server->GetMyGUID(), Reliability::Unreliable);
	net.Step(200);
	EXPECT_FALSE(net.Received(server, kProbe)) << "delivered before the simulated 300 ms";
	EXPECT_TRUE(net.RunUntilReceived(server, kProbe, 1000));
}
