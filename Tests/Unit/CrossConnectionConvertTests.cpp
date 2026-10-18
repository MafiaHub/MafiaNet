/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Two peers on the SimulatedNetwork connect to each other at the same instant
 *  (agreed through an unconnected ping/pong). Crossing handshakes have broken
 *  RakNet in the past; both sides must still end up with one working link.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
const int kRounds = 5;

void PingServer(SimulatedNetwork &net, RakPeer *client, const RakPeer *server)
{
	const SystemAddress target = net.AddressOf(server);
	char host[64];
	target.ToString(false, host, sizeof(host));
	ASSERT_TRUE(client->Ping(host, target.GetPort(), false));
}

bool GotConnectionNotification(SimulatedNetwork &net, const RakPeer *peer)
{
	return net.Received(peer, ID_NEW_INCOMING_CONNECTION) || net.Received(peer, ID_CONNECTION_REQUEST_ACCEPTED);
}

void RunCrossConnectRounds(unsigned short clientMaxIncoming)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1, clientMaxIncoming);
	const SystemAddress serverAddress = net.AddressOf(server);
	const SystemAddress clientAddress = net.AddressOf(client);

	for (int round = 0; round < kRounds; ++round)
	{
		SCOPED_TRACE(testing::Message() << "round " << round);
		net.Inbox(server).clear();
		net.Inbox(client).clear();

		// Ping/pong: the server learns the client's address, the client
		// gets its own send time echoed back.
		const TimeMS pingSentAt = GetTimeMS();
		ASSERT_NO_FATAL_FAILURE(PingServer(net, client, server));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return net.Received(server, ID_UNCONNECTED_PING) && net.Received(client, ID_UNCONNECTED_PONG); }, 1000));
		EXPECT_EQ(clientAddress, net.Find(server, ID_UNCONNECTED_PING)->systemAddress);
		{
			const SimulatedNetwork::Message *pong = net.Find(client, ID_UNCONNECTED_PONG);
			BitStream bs(const_cast<unsigned char *>(pong->data.data()), (unsigned int) pong->data.size(), false);
			bs.IgnoreBytes(1);
			TimeMS echoed = 0;
			ASSERT_TRUE(bs.Read(echoed));
			EXPECT_EQ(pingSentAt, echoed);
		}

		// Both sides wait out the agreed second and connect in the same tick.
		net.Step(1000);
		EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(server, client));
		EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));

		net.RunUntil([&]
		             { return client->GetConnectionState(serverAddress) == IS_CONNECTED &&
			                  server->GetConnectionState(clientAddress) == IS_CONNECTED; }, 2000);
		EXPECT_TRUE(GotConnectionNotification(net, server) || GotConnectionNotification(net, client))
		    << "neither side was told about a connection";
		EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(serverAddress));
		EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(clientAddress));
		EXPECT_EQ(server->GetMyGUID(), client->GetGuidFromSystemAddress(serverAddress));
		EXPECT_EQ(client->GetMyGUID(), server->GetGuidFromSystemAddress(clientAddress));
		ASSERT_FALSE(testing::Test::HasFailure());

		// Tear down for the next round.
		client->CancelConnectionAttempt(serverAddress);
		server->CancelConnectionAttempt(clientAddress);
		server->CloseConnection(server->GetSystemAddressFromIndex(0), true, 0);
		client->CloseConnection(client->GetSystemAddressFromIndex(0), true, 0);
		net.Step(1000);
		for (const ConnectionState state : {client->GetConnectionState(serverAddress), server->GetConnectionState(clientAddress)})
			ASSERT_TRUE(state == IS_DISCONNECTED || state == IS_NOT_CONNECTED) << "state " << state;
	}
}
}

TEST(CrossConnectionConvertSim, SimultaneousConnectWhenOnlyOneSideAcceptsIncoming)
{
	RunCrossConnectRounds(0);
}

TEST(CrossConnectionConvertSim, SimultaneousConnectWhenBothSidesAcceptIncoming)
{
	RunCrossConnectRounds(1);
}
