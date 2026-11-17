/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  MessageFilter, RelayPlugin and TwoWayAuthentication keep their tables in
 *  std:: containers. These cases pin the behaviour those tables carry: filter
 *  set order, membership, room lists, and identifier lookups. Peer-level cases
 *  run on the simulated network.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_filter.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer.h"
#include "mafianet/relay_plugin.h"
#include "mafianet/rpc.h"
#include "mafianet/two_way_authentication.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace MafiaNet;

// AddressOrGUID::operator== matches on GUID or address, which no hash can honour, so there is no
// std::hash for it (a disabled std::hash is not default constructible). MessageFilter hashes privately.
static_assert(!std::is_default_constructible_v<std::hash<AddressOrGUID>>, "AddressOrGUID must not have a public std::hash");
static_assert(std::is_default_constructible_v<std::hash<RakNetGUID>>, "RakNetGUID keeps its std::hash");
static_assert(!std::is_copy_constructible_v<MessageFilter>, "MessageFilter owns its filter sets");
static_assert(!std::is_copy_assignable_v<MessageFilter>, "MessageFilter owns its filter sets");
static_assert(!std::is_copy_constructible_v<RelayPlugin>, "RelayPlugin owns its participants");
static_assert(!std::is_copy_assignable_v<RelayPlugin>, "RelayPlugin owns its participants");
static_assert(!std::is_copy_constructible_v<TwoWayAuthentication>, "TwoWayAuthentication owns its nonces");
static_assert(!std::is_copy_assignable_v<TwoWayAuthentication>, "TwoWayAuthentication owns its nonces");
static_assert(!std::is_copy_constructible_v<TwoWayAuthentication::NonceGenerator>, "NonceGenerator owns its nonces");

namespace
{
std::vector<int> FilterSetIds(MessageFilter &filter)
{
	std::vector<int> ids;
	for (unsigned i = 0; i < filter.GetFilterSetCount(); i++)
		ids.push_back(filter.GetFilterSetIDByIndex(i));
	return ids;
}

void CountingFunction(BitStream *, Packet *, void *context)
{
	++*static_cast<int *>(context);
}
} // namespace

TEST(MessageFilter, FilterSetsAreIndexedInIdOrder)
{
	MessageFilter filter;
	filter.SetAllowMessageID(true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 5);
	filter.SetAllowMessageID(true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 1);
	filter.SetAllowMessageID(true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 3);
	filter.SetAllowMessageID(false, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 3); // existing set, no new one
	EXPECT_EQ((std::vector<int>{1, 3, 5}), FilterSetIds(filter));

	filter.DeleteFilterSet(3);
	EXPECT_EQ((std::vector<int>{1, 5}), FilterSetIds(filter));
	filter.DeleteFilterSet(4); // unknown: no-op
	EXPECT_EQ((std::vector<int>{1, 5}), FilterSetIds(filter));
}

TEST(MessageFilter, SystemMembershipFollowsTheFilterSets)
{
	MessageFilter filter;
	const RakNetGUID a(1), b(2), c(3);
	filter.SetSystemFilterSet(a, 1);
	filter.SetSystemFilterSet(b, 1);
	filter.SetSystemFilterSet(c, 7);
	EXPECT_EQ(3u, filter.GetSystemCount(-1));
	EXPECT_EQ(2u, filter.GetSystemCount(1));
	EXPECT_EQ(1u, filter.GetSystemCount(7));
	EXPECT_EQ(7, filter.GetSystemFilterSet(c));

	filter.SetSystemFilterSet(b, 7); // move
	EXPECT_EQ(1u, filter.GetSystemCount(1));
	EXPECT_EQ(2u, filter.GetSystemCount(7));

	// Deleting a set leaves its systems unrestricted.
	filter.DeleteFilterSet(7);
	EXPECT_EQ(-1, filter.GetSystemFilterSet(b));
	EXPECT_EQ(-1, filter.GetSystemFilterSet(c));
	EXPECT_EQ(1u, filter.GetSystemCount(-1));

	filter.SetSystemFilterSet(a, -1); // remove
	EXPECT_EQ(0u, filter.GetSystemCount(-1));
	EXPECT_EQ(-1, filter.GetSystemFilterSet(a));
}

// A system registered by GUID is found by any key carrying that GUID, with or without an address:
// the private hasher uses the GUID whenever one is set.
TEST(MessageFilter, SystemRegisteredByGuidIsFoundByGuidAndAddressKeys)
{
	MessageFilter filter;
	const RakNetGUID guid(77);
	filter.SetSystemFilterSet(guid, 4);

	AddressOrGUID withAddress;
	withAddress.rakNetGuid = guid;
	withAddress.systemAddress = SystemAddress("10.0.0.9", 1234);
	EXPECT_EQ(4, filter.GetSystemFilterSet(withAddress));
	EXPECT_EQ(4, filter.GetSystemFilterSet(guid));

	filter.SetSystemFilterSet(withAddress, -1); // removal through the fuller key
	EXPECT_EQ(-1, filter.GetSystemFilterSet(guid));
	EXPECT_EQ(0u, filter.GetSystemCount(-1));
}

class MessagingPluginsNetwork : public ::testing::Test
{
protected:
	SimulatedNetwork net;
	RakPeer *server = nullptr;
	std::vector<RakPeer *> clients;
	// Plugins live as long as the fixture, so TearDown can detach them.
	MessageFilter filter;
	RelayPlugin serverRelay, relayA, relayB;
	TwoWayAuthentication serverAuth, clientAuth;
	std::vector<std::pair<RakPeer *, PluginInterface2 *>> attached;

	void Start(int clientCount)
	{
		server = net.AddPeer((unsigned) clientCount, (unsigned short) clientCount);
		ASSERT_NE(nullptr, server);
		for (int i = 0; i < clientCount; i++)
		{
			clients.push_back(net.AddPeer(1));
			ASSERT_NE(nullptr, clients.back());
		}
	}
	void Attach(RakPeer *peer, PluginInterface2 *plugin)
	{
		peer->AttachPlugin(plugin);
		attached.emplace_back(peer, plugin);
	}
	void ConnectAll()
	{
		for (RakPeer *client : clients)
			ASSERT_TRUE(net.Connect(client, server));
	}
	void TearDown() override
	{
		for (auto &entry : attached)
			entry.first->DetachPlugin(entry.second);
	}
	size_t CountRelay(RakPeer *peer, unsigned char event)
	{
		size_t n = 0;
		for (const SimulatedNetwork::Message &m : net.Inbox(peer))
			n += m.data.size() >= 2 && m.data[0] == ID_RELAY_PLUGIN && m.data[1] == event;
		return n;
	}
	// Room name and member count of the last group list the peer received.
	std::vector<std::pair<std::string, int>> LastGroupList(RakPeer *peer)
	{
		std::vector<std::pair<std::string, int>> rooms;
		for (const SimulatedNetwork::Message &m : net.Inbox(peer))
		{
			if (m.data.size() < 2 || m.data[0] != ID_RELAY_PLUGIN || m.data[1] != RPE_GET_GROUP_LIST_REPLY_FROM_SERVER)
				continue;
			rooms.clear();
			BitStream in(const_cast<unsigned char *>(m.data.data()), (unsigned) m.data.size(), false);
			in.IgnoreBytes(2);
			uint16_t count = 0;
			in.Read(count);
			for (uint16_t i = 0; i < count; i++)
			{
				RakString name;
				uint16_t users = 0;
				in.ReadCompressed(name);
				in.Read(users);
				rooms.emplace_back(name.C_String(), users);
			}
		}
		return rooms;
	}
};

TEST_F(MessagingPluginsNetwork, MessageFilterBlocksDisallowedIdsAndRpcNames)
{
	Start(1);
	RPC &serverRpc = server->GetRPC();
	RPC &clientRpc = clients[0]->GetRPC();
	Attach(server, &filter);
	filter.SetAutoAddNewConnectionsToFilter(0);
	filter.SetAllowMessageID(true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 0);
	filter.SetAllowRPC(true, "allowed%s", 0);
	filter.SetAllowRPC(true, "revoked", 0);
	filter.SetAllowRPC(false, "revoked", 0);
	int allowedCalls = 0, revokedCalls = 0;
	ASSERT_TRUE(serverRpc.RegisterFunction("allowed%s", CountingFunction, &allowedCalls));
	ASSERT_TRUE(serverRpc.RegisterFunction("revoked", CountingFunction, &revokedCalls));
	ConnectAll();
	ASSERT_EQ(0, filter.GetSystemFilterSet(clients[0]->GetMyGUID()));

	clientRpc.Call("revoked", nullptr, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	clientRpc.Call("allowed%s", nullptr, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	BitStream blocked;
	blocked.Write((MessageID) (ID_USER_PACKET_ENUM + 1));
	clients[0]->Send(&blocked, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	BitStream allowed;
	allowed.Write((MessageID) ID_USER_PACKET_ENUM);
	clients[0]->Send(&allowed, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);

	// Reliable ordered: once the last message arrives, the earlier ones were processed.
	ASSERT_TRUE(net.RunUntilReceived(server, ID_USER_PACKET_ENUM, 2000));
	EXPECT_EQ(1, allowedCalls);
	EXPECT_EQ(0, revokedCalls);
	EXPECT_FALSE(net.Received(server, ID_USER_PACKET_ENUM + 1));
}

TEST_F(MessagingPluginsNetwork, RelayGroupsAreListedJoinedAndRemoved)
{
	Start(2);
	Attach(server, &serverRelay);
	Attach(clients[0], &relayA);
	Attach(clients[1], &relayB);
	serverRelay.SetAcceptAddParticipantRequests(true);
	ConnectAll();
	const RakNetGUID serverGuid = server->GetMyGUID();

	relayA.AddParticipantRequestFromClient("alice", serverGuid);
	relayB.AddParticipantRequestFromClient("bob", serverGuid);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[0], RPE_ADD_CLIENT_SUCCESS) == 1 && CountRelay(clients[1], RPE_ADD_CLIENT_SUCCESS) == 1; }, 2000));

	// A taken name is refused.
	relayB.AddParticipantRequestFromClient("alice", serverGuid);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[1], RPE_ADD_CLIENT_NAME_ALREADY_IN_USE) == 1; }, 2000));

	relayA.JoinGroupRequest(serverGuid, "lobby");
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[0], RPE_JOIN_GROUP_SUCCESS) == 1; }, 2000));
	relayB.JoinGroupRequest(serverGuid, "lobby");
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[1], RPE_JOIN_GROUP_SUCCESS) == 1 && CountRelay(clients[0], RPE_USER_ENTERED_ROOM) == 1; }, 2000));

	relayA.GetGroupList(serverGuid);
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_RELAY_PLUGIN, 2000));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !LastGroupList(clients[0]).empty(); }, 2000));
	EXPECT_EQ((std::vector<std::pair<std::string, int>>{{"lobby", 2}}), LastGroupList(clients[0]));

	// Routing by name goes through the name table.
	BitStream payload;
	payload.Write((unsigned char) 0x5A);
	relayB.SendToParticipant(serverGuid, "alice", &payload, Priority::High, Reliability::ReliableOrdered, 0);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[0], RPE_MESSAGE_TO_CLIENT_FROM_SERVER) == 1; }, 2000));

	relayB.LeaveGroup(serverGuid);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[0], RPE_USER_LEFT_ROOM) == 1; }, 2000));

	// The last member leaving deletes the room.
	relayA.LeaveGroup(serverGuid);
	net.Inbox(clients[0]).clear();
	relayA.GetGroupList(serverGuid);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[0], RPE_GET_GROUP_LIST_REPLY_FROM_SERVER) == 1; }, 2000));
	EXPECT_TRUE(LastGroupList(clients[0]).empty());

	// A participant that leaves frees its name.
	relayA.RemoveParticipantRequestFromClient(serverGuid);
	relayB.AddParticipantRequestFromClient("alice", serverGuid);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return CountRelay(clients[1], RPE_ADD_CLIENT_SUCCESS) == 2; }, 2000));
}

TEST_F(MessagingPluginsNetwork, TwoWayAuthenticationMatchesIdentifiersVerbatim)
{
	Start(1);
	Attach(server, &serverAuth);
	Attach(clients[0], &clientAuth);
	ConnectAll();

	// '%' in an identifier: the received identifier used to go through RakString's printf-style constructor.
	// Built by assignment: passing the literal to the RakString parameter would itself format it.
	RakString identifier;
	identifier = "build%s";
	ASSERT_TRUE(serverAuth.AddPassword(identifier, "secret"));
	ASSERT_FALSE(serverAuth.AddPassword(identifier, "other"));
	ASSERT_TRUE(clientAuth.AddPassword(identifier, "secret"));
	ASSERT_TRUE(clientAuth.AddPassword("wrong", "nope"));
	ASSERT_TRUE(serverAuth.AddPassword("wrong", "different"));
	EXPECT_FALSE(clientAuth.Challenge("unknown", server->GetMyGUID()));

	ASSERT_TRUE(clientAuth.Challenge(identifier, server->GetMyGUID()));
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS, 2000));
	ASSERT_TRUE(net.RunUntilReceived(server, ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS, 2000));
	EXPECT_TRUE(clientAuth.outgoingChallenges.empty());

	ASSERT_TRUE(clientAuth.Challenge("wrong", server->GetMyGUID()));
	ASSERT_TRUE(net.RunUntilReceived(server, ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE, 2000));
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE, 2000));
	EXPECT_TRUE(clientAuth.outgoingChallenges.empty());
}
