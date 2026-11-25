// Copyright (c) 2026, MafiaHub. MIT license.
#include "../../Source/src/player_roster.h"

#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/snapshot_replica.h"
#include <gtest/gtest.h>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_convertible_v<PlayerId, NetworkID>);
static_assert(!std::is_convertible_v<PeerGuid, PlayerId>);

TEST(PlayerRoster, WrapSkipsActiveNumbersAndExhaustionPreservesJoins)
{
	PlayerRoster players(3);
	for (uint32_t i = 1; i <= 5; ++i)
		players.Connected(PeerGuid{i});
	EXPECT_EQ(players.Accept(PeerGuid{1}), PlayerId{1});
	EXPECT_EQ(players.Accept(PeerGuid{2}), PlayerId{2});
	EXPECT_EQ(players.Disconnected(PeerGuid{2}), PlayerId{2});
	EXPECT_EQ(players.Accept(PeerGuid{3}), PlayerId{3});
	EXPECT_EQ(players.Accept(PeerGuid{4}), PlayerId{2});
	EXPECT_FALSE(players.Accept(PeerGuid{5}));
	EXPECT_EQ(players.Accept(PeerGuid{4}), PlayerId{2});
}

TEST(PlayerRoster, UnassignedGuidCannotBecomeAPlayer)
{
	PlayerRoster players;
	players.Connected(UNASSIGNED_PEER_GUID);
	EXPECT_FALSE(players.Accept(UNASSIGNED_PEER_GUID));
	EXPECT_FALSE(players.FindId(UNASSIGNED_PEER_GUID));
}

TEST(PeerPlayers, DeferredSessionCannotJoinBeforeAdmission)
{
	SimulatedNetwork net;
	net.onSessionRequest = {};
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	const auto guid = ToPeerGuid(client->GetMyGUID());

	ASSERT_EQ(net.StartConnect(client, server), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_SESSION_CONFIG_REQUEST, 5000));
	EXPECT_FALSE(server->AcceptPlayer(guid));
	server->AcceptSession(ToGuid(guid), nullptr, 0);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_NEW_INCOMING_CONNECTION, 5000));
	EXPECT_FALSE(server->GetPlayerId(guid));
	EXPECT_EQ(server->AcceptPlayer(guid), PlayerId{1});
}

namespace
{
struct PropState
{
	void Write(BitStream &out) const { out.Write(true); }
	bool Read(BitStream &in)
	{
		bool present;
		return in.Read(present) && present;
	}
};
using Prop = SnapshotReplica<PropState>;

}

TEST(PeerPlayers, AcceptedGameJoinOwnsTheNumberAcrossAvatarChanges)
{
	std::vector<Prop> scenery(4005);
	Prop avatar, replacement;
	SimulatedNetwork net;
	auto *server = net.AddPeer(2, 2);
	auto *client = net.AddPeer(1);
	auto *pending = net.AddPeer(1);
	const auto guid = ToPeerGuid(client->GetMyGUID());
	const auto pendingGuid = ToPeerGuid(pending->GetMyGUID());
	auto &world = server->GetReplication();
	ASSERT_TRUE(world.SetRole(ReplicationRole::Server));
	world.SetAutomaticAdmission(false);
	ASSERT_TRUE(world.RegisterReplica<Prop>(ReplicaTypeId{1}, 1));
	for (auto &prop : scenery)
		ASSERT_TRUE(world.Reference(&prop));

	EXPECT_FALSE(server->AcceptPlayer(guid));
	ASSERT_TRUE(net.Connect(pending, server));
	pending->CloseConnection(net.AddressOf(server), true);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 5000));
	EXPECT_EQ(net.Find(server, ID_DISCONNECTION_NOTIFICATION)->playerId, PlayerId{});
	EXPECT_FALSE(server->AcceptPlayer(pendingGuid));
	ASSERT_TRUE(net.Connect(client, server));
	EXPECT_FALSE(client->AcceptPlayer(ToPeerGuid(server->GetMyGUID())));
	EXPECT_FALSE(server->GetPlayerId(guid));
	EXPECT_EQ(server->AcceptPlayer(guid), PlayerId{1});
	EXPECT_EQ(server->AcceptPlayer(guid), PlayerId{1});
	ASSERT_TRUE(world.Reference(&avatar));
	const auto oldEntity = avatar.GetNetworkID();
	ASSERT_TRUE(world.Destroy(&avatar));
	ASSERT_TRUE(world.Reference(&replacement));
	EXPECT_NE(replacement.GetNetworkID(), oldEntity);
	EXPECT_EQ(server->GetPlayerId(guid), PlayerId{1});
	EXPECT_EQ(server->GetPlayerGuid(PlayerId{1}), guid);
}

TEST(PeerPlayers, QueuedKickNotificationKeepsOldIdAfterSameGuidReconnects)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	const auto guid = ToPeerGuid(client->GetMyGUID());
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_EQ(server->AcceptPlayer(guid), PlayerId{1});
	net.SetReceiveEnabled(server, false);
	server->CloseConnection(ToGuid(guid), true);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_DISCONNECTION_NOTIFICATION, 5000));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !server->GetPlayerId(guid); }, 5000));
	net.Inbox(client).clear();
	net.SetReceiveEnabled(server, true);
	ASSERT_EQ(net.StartConnect(client, server), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	ASSERT_EQ(server->AcceptPlayer(guid), PlayerId{2});

	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 5000));
	EXPECT_EQ(net.Find(server, ID_DISCONNECTION_NOTIFICATION)->playerId, PlayerId{1});
	EXPECT_FALSE(server->GetPlayerGuid(PlayerId{1}));
	EXPECT_EQ(server->GetPlayerId(guid), PlayerId{2});
}

TEST(PeerPlayers, RosterSnapshotsSurviveSilentCloseRemoteDisconnectAndShutdown)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(3, 3);
	auto *first = net.AddPeer(1);
	auto *second = net.AddPeer(1);
	auto *third = net.AddPeer(1);
	const auto firstGuid = ToPeerGuid(first->GetMyGUID());
	const auto secondGuid = ToPeerGuid(second->GetMyGUID());
	const auto thirdGuid = ToPeerGuid(third->GetMyGUID());
	ASSERT_TRUE(net.Connect(first, server));
	ASSERT_TRUE(net.Connect(second, server));
	ASSERT_TRUE(net.Connect(third, server));
	ASSERT_EQ(server->AcceptPlayer(firstGuid), PlayerId{1});
	ASSERT_EQ(server->AcceptPlayer(secondGuid), PlayerId{2});
	ASSERT_EQ(server->AcceptPlayer(thirdGuid), PlayerId{3});
	std::vector<std::pair<PlayerId, PeerGuid>> roster;
	server->GetPlayerList(roster);
	server->CloseConnection(ToGuid(firstGuid), false);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_CONNECTION_LOST, 5000));
	EXPECT_EQ(net.Find(server, ID_CONNECTION_LOST)->playerId, PlayerId{1});
	second->CloseConnection(net.AddressOf(server), true);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 5000));
	EXPECT_EQ(net.Find(server, ID_DISCONNECTION_NOTIFICATION)->playerId, PlayerId{2});
	EXPECT_EQ(roster, (decltype(roster){{PlayerId{1}, firstGuid}, {PlayerId{2}, secondGuid}, {PlayerId{3}, thirdGuid}}));
	server->GetPlayerList(roster);
	EXPECT_EQ(roster, (decltype(roster){{PlayerId{3}, thirdGuid}}));
	const auto address = net.AddressOf(server);
	server->Shutdown(0);
	server->GetPlayerList(roster);
	EXPECT_TRUE(roster.empty());
	EXPECT_FALSE(server->AcceptPlayer(thirdGuid));

	char host[64];
	address.ToString(false, host, sizeof(host));
	SocketDescriptor descriptor(address.GetPort(), host);
	ASSERT_EQ(server->Startup(1, &descriptor, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(1);
	auto *fresh = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(fresh, server));
	EXPECT_EQ(server->AcceptPlayer(ToPeerGuid(fresh->GetMyGUID())), PlayerId{1});
}

TEST(PeerPlayers, TimeoutRetiresPlayerBeforeNotificationIsConsumed)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	server->SetTimeoutTime(2000, UNASSIGNED_SYSTEM_ADDRESS);
	ASSERT_TRUE(net.Connect(client, server));
	const auto guid = ToPeerGuid(client->GetMyGUID());
	ASSERT_EQ(server->AcceptPlayer(guid), PlayerId{1});
	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetLinkBothWays(client, server, dark);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_CONNECTION_LOST, 20000));
	EXPECT_EQ(net.Find(server, ID_CONNECTION_LOST)->playerId, PlayerId{1});
	EXPECT_FALSE(server->AcceptPlayer(guid));
	EXPECT_FALSE(server->GetPlayerGuid(PlayerId{1}));
}

TEST(PeerPlayers, CrossedPlaintextConnectionsHaveOneAcceptingPeer)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	auto *first = net.AddPeer(1, 1);
	auto *second = net.AddPeer(1, 1);
	const auto firstGuid = ToPeerGuid(first->GetMyGUID());
	const auto secondGuid = ToPeerGuid(second->GetMyGUID());

	ASSERT_EQ(net.StartConnect(first, second), CONNECTION_ATTEMPT_STARTED);
	ASSERT_EQ(net.StartConnect(second, first), CONNECTION_ATTEMPT_STARTED);
	auto *accepting = first->GetMyGUID().g > second->GetMyGUID().g ? first : second;
	auto *connecting = accepting == first ? second : first;
	ASSERT_TRUE(net.RunUntilReceived(accepting, ID_NEW_INCOMING_CONNECTION, 5000));
	ASSERT_TRUE(net.RunUntilReceived(connecting, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	EXPECT_TRUE(net.Received(accepting, ID_SESSION_CONFIG_REQUEST));
	EXPECT_FALSE(net.Received(connecting, ID_SESSION_CONFIG_REQUEST));
	EXPECT_FALSE(connecting->AcceptPlayer(ToPeerGuid(accepting->GetMyGUID())));

	first->CloseConnection(ToGuid(secondGuid), true);
	ASSERT_TRUE(net.RunUntilReceived(first, ID_DISCONNECTION_NOTIFICATION, 5000));
	ASSERT_TRUE(net.RunUntilReceived(second, ID_DISCONNECTION_NOTIFICATION, 5000));
	EXPECT_FALSE(first->GetPlayerId(secondGuid));
	EXPECT_FALSE(second->GetPlayerId(firstGuid));

	net.Inbox(first).clear();
	net.Inbox(second).clear();
	ASSERT_TRUE(net.Connect(second, first));
	EXPECT_EQ(first->AcceptPlayer(secondGuid), PlayerId{1});
	EXPECT_FALSE(second->AcceptPlayer(firstGuid));
}
