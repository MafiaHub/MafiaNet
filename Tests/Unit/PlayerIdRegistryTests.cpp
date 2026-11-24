// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/player_ids.h"

#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/snapshot_replica.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_convertible_v<PlayerId, NetworkID>);
static_assert(!std::is_convertible_v<PeerGuid, PlayerId>);

TEST(PlayerIdRegistry, AssignsOncePerJoinAndResolvesBothDirections)
{
	PlayerIdRegistry players;
	const PeerGuid first{81}, second{29};
	EXPECT_FALSE(players.FindId(first));
	EXPECT_FALSE(players.FindPeer(PlayerId{}));
	EXPECT_FALSE(players.Assign(UNASSIGNED_PEER_GUID));
	EXPECT_EQ(players.Assign(first), PlayerId{1});
	EXPECT_EQ(players.Assign(first), PlayerId{1});
	EXPECT_EQ(players.Assign(second), PlayerId{2});
	EXPECT_EQ(players.FindId(first), PlayerId{1});
	EXPECT_EQ(players.FindPeer(PlayerId{2}), second);

	EXPECT_TRUE(players.Release(PlayerId{1}));
	EXPECT_FALSE(players.FindId(first));
	EXPECT_FALSE(players.FindPeer(PlayerId{1}));
	EXPECT_EQ(players.Assign(first), PlayerId{3});
	EXPECT_FALSE(players.Release(PlayerId{1}));
	EXPECT_EQ(players.FindId(first), PlayerId{3});
	EXPECT_EQ(players.FindPeer(PlayerId{3}), first);
	EXPECT_EQ(players.FindId(second), PlayerId{2});
}

TEST(PlayerIdRegistry, WrapSkipsLiveIdsAndExhaustionPreservesExistingJoins)
{
	PlayerIdRegistry players(3);
	EXPECT_EQ(players.Assign(PeerGuid{10}), PlayerId{1});
	EXPECT_EQ(players.Assign(PeerGuid{20}), PlayerId{2});
	EXPECT_TRUE(players.Release(PlayerId{2}));
	EXPECT_EQ(players.Assign(PeerGuid{30}), PlayerId{3});
	EXPECT_EQ(players.Assign(PeerGuid{40}), PlayerId{2});
	EXPECT_FALSE(players.Assign(PeerGuid{50}));
	EXPECT_EQ(players.Assign(PeerGuid{40}), PlayerId{2});
	EXPECT_EQ(players.FindPeer(PlayerId{1}), PeerGuid{10});
	EXPECT_TRUE(players.Release(PlayerId{3}));
	EXPECT_EQ(players.Assign(PeerGuid{50}), PlayerId{3});
}

TEST(PlayerIdRegistry, EmptyIdRangeAssignsNothing)
{
	PlayerIdRegistry players(0);
	EXPECT_FALSE(players.Assign(PeerGuid{10}));
	EXPECT_FALSE(players.Release(PlayerId{}));
}

TEST(PlayerIdRegistry, PlayerListReplacesOutputWithAnOrderedSnapshotOfActiveJoins)
{
	PlayerIdRegistry players(3);
	std::vector<std::pair<PlayerId, PeerGuid>> roster{{PlayerId{9}, PeerGuid{99}}};
	players.GetPlayerList(roster);
	EXPECT_TRUE(roster.empty());
	ASSERT_EQ(players.Assign(PeerGuid{71}), PlayerId{1});
	ASSERT_EQ(players.Assign(PeerGuid{32}), PlayerId{2});
	ASSERT_EQ(players.Assign(PeerGuid{29}), PlayerId{3});
	ASSERT_TRUE(players.Release(PlayerId{2}));
	ASSERT_EQ(players.Assign(PeerGuid{90}), PlayerId{2});
	players.GetPlayerList(roster);
	ASSERT_TRUE(players.Release(PlayerId{1}));
	EXPECT_EQ(roster, (decltype(roster){{PlayerId{1}, PeerGuid{71}}, {PlayerId{2}, PeerGuid{90}}, {PlayerId{3}, PeerGuid{29}}}));
	players.GetPlayerList(roster);
	EXPECT_EQ(roster, (decltype(roster){{PlayerId{2}, PeerGuid{90}}, {PlayerId{3}, PeerGuid{29}}}));
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

// Consume the notification so a reconnect must produce a fresh event.
PeerGuid TakeEvent(SimulatedNetwork &net, RakPeer *peer, MessageID id)
{
	if (!net.RunUntilReceived(peer, id, 5000))
		return UNASSIGNED_PEER_GUID;
	auto &inbox = net.Inbox(peer);
	const auto event = std::find_if(inbox.begin(), inbox.end(), [id](const auto &message)
	                                { return message.id == id; });
	const auto guid = ToPeerGuid(event->guid);
	inbox.erase(event);
	return guid;
}
}

TEST(PlayerIdRegistry, GameJoinAndDisconnectEventsOwnTheIdAcrossAvatarChanges)
{
	std::vector<Prop> scenery(4005);
	Prop avatar, replacement;
	SimulatedNetwork net;
	auto *server = net.AddPeer(3, 3);
	auto *first = net.AddPeer(1);
	auto *second = net.AddPeer(1);
	auto *pending = net.AddPeer(1);
	const auto firstGuid = ToPeerGuid(first->GetMyGUID());
	const auto secondGuid = ToPeerGuid(second->GetMyGUID());
	auto &world = server->GetReplication();
	ASSERT_TRUE(world.SetRole(ReplicationRole::Server));
	world.SetAutomaticAdmission(false);
	ASSERT_TRUE(world.RegisterReplica<Prop>(ReplicaTypeId{1}, 1));
	for (auto &prop : scenery)
		ASSERT_TRUE(world.Reference(&prop));
	PlayerIdRegistry players;

	// This transport connection leaves before the application accepts its game join.
	ASSERT_TRUE(net.Connect(pending, server));
	ASSERT_EQ(TakeEvent(net, server, ID_NEW_INCOMING_CONNECTION), ToPeerGuid(pending->GetMyGUID()));
	pending->CloseConnection(net.AddressOf(server), true);
	const auto abandoned = TakeEvent(net, server, ID_DISCONNECTION_NOTIFICATION);
	ASSERT_EQ(abandoned, ToPeerGuid(pending->GetMyGUID()));
	EXPECT_FALSE(players.FindId(abandoned));

	ASSERT_TRUE(net.Connect(first, server));
	ASSERT_EQ(players.Assign(TakeEvent(net, server, ID_NEW_INCOMING_CONNECTION)), PlayerId{1});
	ASSERT_TRUE(net.Connect(second, server));
	ASSERT_EQ(players.Assign(TakeEvent(net, server, ID_NEW_INCOMING_CONNECTION)), PlayerId{2});
	ASSERT_TRUE(world.Reference(&avatar));
	const auto oldEntity = avatar.GetNetworkID();
	ASSERT_TRUE(world.Destroy(&avatar));
	ASSERT_TRUE(world.Reference(&replacement));
	EXPECT_NE(replacement.GetNetworkID(), oldEntity);
	EXPECT_EQ(players.FindId(firstGuid), PlayerId{1});

	// A kick requests transport teardown. The server can still resolve the ID in its callback.
	server->CloseConnection(ToGuid(firstGuid), true);
	const auto kicked = TakeEvent(net, server, ID_DISCONNECTION_NOTIFICATION);
	ASSERT_EQ(kicked, firstGuid);
	const auto oldId = players.FindId(kicked);
	ASSERT_EQ(oldId, PlayerId{1});
	EXPECT_EQ(players.FindPeer(*oldId), kicked);
	ASSERT_TRUE(players.Release(*oldId));
	ASSERT_EQ(TakeEvent(net, first, ID_DISCONNECTION_NOTIFICATION), ToPeerGuid(server->GetMyGUID()));
	ASSERT_TRUE(net.Connect(first, server));
	ASSERT_EQ(players.Assign(TakeEvent(net, server, ID_NEW_INCOMING_CONNECTION)), PlayerId{3});
	EXPECT_FALSE(players.Release(*oldId));
	EXPECT_EQ(players.FindPeer(PlayerId{3}), firstGuid);

	second->CloseConnection(net.AddressOf(server), true);
	ASSERT_EQ(TakeEvent(net, server, ID_DISCONNECTION_NOTIFICATION), secondGuid);
	ASSERT_EQ(players.FindId(secondGuid), PlayerId{2});
	EXPECT_TRUE(players.Release(PlayerId{2}));
	EXPECT_FALSE(players.FindPeer(PlayerId{2}));
}
