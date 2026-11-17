/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RakPeer's list-returning public API (GetSystemList, GetStatisticsList,
 *  GetSockets / ReleaseSockets) on std::vector out-params, run on the
 *  simulated network.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/instance_ptr.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_TEST_PAYLOAD = ID_USER_PACKET_ENUM + 11;

void SendPayload(RakPeer *from, const RakNetGUID &to, unsigned int bytes)
{
	BitStream bs;
	bs.Write(ID_TEST_PAYLOAD);
	for (unsigned int i = 1; i < bytes; i++)
		bs.Write(static_cast<unsigned char>(i));
	ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, to, false));
}
} // namespace

TEST(PeerSystemList, GetSystemListReturnsEveryConnectedPeer)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *clients[3] = {net.AddPeer(1), net.AddPeer(1), net.AddPeer(1)};
	for (RakPeer *client : clients)
		ASSERT_TRUE(net.Connect(client, server));

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	// Stale contents must be replaced, not appended to.
	addresses.push_back(UNASSIGNED_SYSTEM_ADDRESS);
	guids.push_back(UNASSIGNED_RAKNET_GUID);
	server->GetSystemList(addresses, guids);

	ASSERT_EQ(addresses.size(), 3u);
	ASSERT_EQ(guids.size(), 3u);
	for (std::size_t i = 0; i < guids.size(); i++)
		EXPECT_EQ(server->GetGuidFromSystemAddress(addresses[i]), guids[i]) << "index " << i;
	for (RakPeer *client : clients)
	{
		const RakNetGUID guid = client->GetMyGUID();
		EXPECT_NE(std::find(guids.begin(), guids.end(), guid), guids.end());
	}

	// A client sees exactly its server.
	clients[0]->GetSystemList(addresses, guids);
	ASSERT_EQ(guids.size(), 1u);
	EXPECT_EQ(guids[0], server->GetMyGUID());
	EXPECT_EQ(addresses[0], net.AddressOf(server));
}

TEST(PeerSystemList, GetStatisticsListIsIndexAlignedWithGuids)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *clients[3] = {net.AddPeer(1), net.AddPeer(1), net.AddPeer(1)};
	for (RakPeer *client : clients)
		ASSERT_TRUE(net.Connect(client, server));

	// A distinct amount of user data to each client, so each connection's
	// statistics identify the client they belong to.
	std::map<RakNetGUID, unsigned int> payloadBytes;
	for (unsigned int k = 0; k < 3; k++)
	{
		payloadBytes[clients[k]->GetMyGUID()] = 1000 * (k + 1);
		SendPayload(server, clients[k]->GetMyGUID(), 1000 * (k + 1));
	}
	for (RakPeer *client : clients)
		ASSERT_TRUE(net.RunUntilReceived(client, ID_TEST_PAYLOAD, 2000));

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	std::vector<RakNetStatistics> statistics;
	server->GetStatisticsList(addresses, guids, statistics);

	ASSERT_EQ(addresses.size(), 3u);
	ASSERT_EQ(guids.size(), 3u);
	ASSERT_EQ(statistics.size(), 3u);
	for (std::size_t i = 0; i < guids.size(); i++)
	{
		ASSERT_EQ(payloadBytes.count(guids[i]), 1u) << "index " << i;
		const uint64_t pushed = statistics[i].runningTotal[USER_MESSAGE_BYTES_PUSHED];
		// The payload plus a few internal messages (connection handshake, pings).
		EXPECT_GE(pushed, payloadBytes[guids[i]]) << "index " << i;
		EXPECT_LT(pushed, payloadBytes[guids[i]] + 500u) << "index " << i;

		RakNetStatistics single;
		ASSERT_NE(server->GetStatistics(addresses[i], &single), nullptr);
		EXPECT_EQ(single.runningTotal[USER_MESSAGE_BYTES_PUSHED], pushed) << "index " << i;
	}
}

TEST(PeerSystemList, GetSocketsReturnsBoundSockets)
{
	SimulatedNetwork net;
	// Declared after net: destroyed (through DestroyInstance) first, even when an ASSERT returns early.
	InstancePtr<RakPeerInterface> owner = MakeInstance<RakPeerInterface>();
	RakPeer *peer = static_cast<RakPeer *>(owner.get());
	peer->SetSocketFactory(&net);
	peer->SetManualUpdate(true);
	SocketDescriptor descriptors[2] = {SocketDescriptor(61000, "10.0.0.200"), SocketDescriptor(61001, "10.0.0.200")};
	ASSERT_EQ(peer->Startup(2, descriptors, 2), RAKNET_STARTED);

	std::vector<RakNetSocket2 *> sockets;
	sockets.push_back(nullptr); // stale contents must be replaced
	peer->GetSockets(sockets);
	ASSERT_EQ(sockets.size(), 2u);
	ASSERT_NE(sockets[0], nullptr);
	ASSERT_NE(sockets[1], nullptr);
	EXPECT_EQ(sockets[0]->GetBoundAddress().GetPort(), 61000);
	EXPECT_EQ(sockets[1]->GetBoundAddress().GetPort(), 61001);

	peer->ReleaseSockets(sockets);
	EXPECT_TRUE(sockets.empty());

	peer->Shutdown(0);
}

TEST(PeerSystemList, PublishesNewAddressForConnectedGuid)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto guid = client->GetMyGUID();
	const SystemAddress newAddress("10.0.1.42", 4242);
	server->ChangeSystemAddress(guid, newAddress);
	net.Step(1);

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	server->GetSystemList(addresses, guids);
	EXPECT_EQ(std::vector<SystemAddress>{newAddress}, addresses);
	EXPECT_EQ(std::vector<RakNetGUID>{guid}, guids);
}

TEST(PeerSystemList, SilentCloseRemovesConnectedSlot)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	server->CloseConnection(net.AddressOf(client), false);
	net.Step(1);

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	server->GetSystemList(addresses, guids);
	EXPECT_TRUE(addresses.empty());
	EXPECT_TRUE(guids.empty());
}
