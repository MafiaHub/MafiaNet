/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <vector>
#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

TEST(SimulatedSoak, ReconnectDuringFragmentedTransfer)
{
	unsigned fragments = 0;
	SimulatedNetwork net(73);
	auto *client = net.AddPeer(1);
	auto *server = net.AddPeer(1, 1);
	const auto serverAddress = net.AddressOf(server);
	const auto clientAddress = net.AddressOf(client);
	ASSERT_TRUE(net.Connect(client, server));

	constexpr auto oldId = ID_USER_PACKET_ENUM;
	constexpr auto newId = ID_USER_PACKET_ENUM + 1;
	for (unsigned epoch = 0; epoch < 24; ++epoch)
	{
		SCOPED_TRACE(epoch);
		SimulatedNetwork::LinkRule delayed;
		delayed.delayMs = 2000;
		delayed.jitterMs = 20;
		net.SetLink(client, server, delayed);
		std::vector<unsigned char> block(64 * 1024, 0xA5);
		block[0] = oldId;
		// Queue a complete old-session message too: stale delivery must be detectable
		// even when the interrupted block never had enough fragments to reassemble.
		const char marker = static_cast<char>(oldId);
		ASSERT_NE(client->Send(&marker, 1, Priority::High, Reliability::ReliableOrdered, 1, serverAddress, false), 0u);
		net.Step(30);
		fragments = 0;
		net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *, int size)
		           {
			if (from == clientAddress && size > 900)
				++fragments; });
		ASSERT_NE(client->Send(reinterpret_cast<const char *>(block.data()), block.size(), Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false), 0u);
		ASSERT_TRUE(net.RunUntil([&]
		                         { return fragments >= 2; }, 500, 1));
		net.SetTap({});
		ASSERT_FALSE(net.Received(server, oldId));

		client->CloseConnection(serverAddress, false);
		server->CloseConnection(clientAddress, false);
		net.Step(10);
		net.Inbox(server).clear();
		net.Inbox(client).clear();
		net.SetLink(client, server, {});
		const auto reconnectStart = net.Now();
		ASSERT_TRUE(net.Connect(client, server, 1000));
		// The old datagrams are still in flight when the new session starts.
		ASSERT_LT(net.Now() - reconnectStart, 1000000u);
		SimulatedNetwork::LinkRule lossy;
		lossy.lossPercent = 8;
		lossy.delayMs = 10;
		lossy.jitterMs = 7;
		net.SetLinkBothWays(client, server, lossy);
		block[0] = newId;
		for (size_t i = 1; i < block.size(); ++i)
			block[i] = static_cast<unsigned char>(i + epoch);
		ASSERT_NE(client->Send(reinterpret_cast<const char *>(block.data()), block.size(), Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false), 0u);
		ASSERT_TRUE(net.RunUntilReceived(server, newId, 15000));
		net.Step(2100); // all delayed fragments from the old session have now arrived
		int received = 0;
		for (const auto &message : net.Inbox(server))
		{
			ASSERT_NE(message.id, oldId);
			if (message.id == newId)
			{
				ASSERT_EQ(message.data, block);
				++received;
			}
		}
		ASSERT_EQ(received, 1);
		ASSERT_EQ(client->NumberOfConnections(), 1u);
		ASSERT_EQ(server->NumberOfConnections(), 1u);
		net.SetLinkBothWays(client, server, {});
		net.Inbox(server).clear();
	}
}
