// Copyright (c) 2026, MafiaHub. MIT license.

#include <gtest/gtest.h>

#include "mafianet/peer_handle.h"
#include "mafianet/replication_manager.h"
#include "mafianet/replica_manager3.h"

#include "mafianet/snapshot_replica.h"
#include "SimulatedNetwork.h"

#include <type_traits>

using namespace MafiaNet;

TEST(BuiltinReplication, EachPeerOwnsAServiceBeforeStartup)
{
	Peer server, client;
	ReplicationManager &replication = server->GetReplication();
	EXPECT_NE(&replication, &client->GetReplication());
	EXPECT_EQ(&server->GetReplication(), &replication);
	EXPECT_TRUE(replication.SetRole(ReplicationRole::Server));
	EXPECT_EQ(replication.GetRole(), ReplicationRole::Server);
	EXPECT_EQ(client->GetReplication().GetRole(), ReplicationRole::Unconfigured);
}

TEST(BuiltinReplication, ConfigurationSurvivesPeerRestart)
{
	Peer peer;
	auto &replication = peer->GetReplication();
	ASSERT_TRUE(replication.SetRole(ReplicationRole::Client));
	for (unsigned int cycle = 0; cycle < 2; ++cycle)
	{
		SocketDescriptor socket(0, "127.0.0.1");
		ASSERT_EQ(peer->Startup(1, &socket, 1), RAKNET_STARTED);
		EXPECT_EQ(&peer->GetReplication(), &replication);
		peer->Shutdown(0);
		EXPECT_EQ(replication.GetRole(), ReplicationRole::Client);
		EXPECT_EQ(replication.GetReplicaCount(), 0u);
	}
}

TEST(BuiltinReplication, BothReplicationInterfacesAreIndependentOfPlugins)
{
	EXPECT_FALSE((std::is_base_of_v<PluginInterface2, ReplicationManager>) );
	EXPECT_FALSE((std::is_base_of_v<PluginInterface2, ReplicaManager3>) );
}

namespace
{
constexpr ReplicaTypeId doorType{17};
struct DoorState
{
	bool open = false;
	void Write(BitStream &out) const { out.Write(open); }
	bool Read(BitStream &in) { return in.Read(open); }
};
struct Door : SnapshotReplica<DoorState>
{
};
}

TEST(BuiltinReplication, RegistersStableTypesAndDeliversDoorThroughThePeer)
{
	SimulatedNetwork net(17);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication();
	auto &view = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(host.RegisterReplica<Door>(doorType, 1));
	EXPECT_FALSE(host.RegisterReplica<Door>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<Door>(doorType, 1));
	Door door;
	door.state.open = true;
	ASSERT_TRUE(host.Reference(&door));
	const auto id = door.GetNetworkID();
	ASSERT_TRUE(net.Connect(client, server));
	auto copy = [&]
	{ return dynamic_cast<Door *>(view.FindReplica(id)); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() && copy()->state.open; }, 5000));
	door.state.open = false;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() && !copy()->state.open; }, 5000));
	EXPECT_TRUE(host.Destroy(&door));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !copy(); }, 5000));
	EXPECT_FALSE(door.WasReferenced());
}

TEST(BuiltinReplication, IncompatibleSchemaWithholdsConstruction)
{
	SimulatedNetwork net(18);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication();
	auto &view = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(host.RegisterReplica<Door>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<Door>(doorType, 2));
	Door door;
	ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetPeerStatus(ToPeerGuid(server->GetMyGUID())) == ReplicationPeerStatus::SchemaMismatch; }, 5000));
	EXPECT_EQ(view.GetReplicaCount(), 0u);
}

TEST(BuiltinReplication, StaticBindingsUpdateExistingObjects)
{
	struct Room : Door
	{
		bool publishes = false;
		RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return publishes ? RM3CS_ALREADY_EXISTS_REMOTELY : RM3CS_NEVER_CONSTRUCT; }
		void SerializeConstructionExisting(BitStream *out, Connection_RM3 *connection) override { SerializeConstruction(out, connection); }
		void DeserializeConstructionExisting(BitStream *in, Connection_RM3 *connection) override { EXPECT_TRUE(DeserializeConstruction(in, connection)); }
		bool QueryRemoteConstruction(Connection_RM3 *) override { return !publishes; }
	};
	SimulatedNetwork net(21);
	auto *one = net.AddPeer(1, 1);
	auto *two = net.AddPeer(1);
	Room source, sink;
	source.publishes = source.state.open = true;
	for (auto *peer : {one, two})
	{
		auto &service = peer->GetReplication();
		ASSERT_TRUE(service.SetRole(ReplicationRole::Peer));
		ASSERT_TRUE(service.RegisterReplica<Room>(doorType, 1));
		ASSERT_TRUE(service.ReferenceStatic(peer == one ? &source : &sink, 0));
	}
	ASSERT_TRUE(net.Connect(two, one));
	EXPECT_TRUE(net.RunUntil([&]
	                         { return sink.state.open; }, 5000));
	EXPECT_EQ(two->GetReplication().FindReplica(0), &sink);
}

TEST(BuiltinReplication, RadiusAndRoomSubscribersStreamIndependentlyAcrossReentry)
{
	SimulatedNetwork net(42);
	auto *server = net.AddPeer(2, 2);
	auto *camera = net.AddPeer(1);
	auto *room = net.AddPeer(1);
	for (auto *peer : {server, camera, room})
	{
		auto &service = peer->GetReplication();
		ASSERT_TRUE(service.SetRole(peer == server ? ReplicationRole::Server : ReplicationRole::Client));
		ASSERT_TRUE(service.RegisterReplica<Door>(doorType, 1));
	}
	auto &host = server->GetReplication();
	auto &grid = host.GetInterestGrid();
	ASSERT_TRUE(grid.Configure({{0, 0}, {160, 64}, 64}));
	const auto cameraGuid = ToPeerGuid(camera->GetMyGUID());
	const auto roomGuid = ToPeerGuid(room->GetMyGUID());
	ASSERT_TRUE(grid.SetObserverRadius(cameraGuid, {63, 20}, 10, 15));
	ASSERT_TRUE(grid.SetObserverCells(roomGuid, {{1, 0}}));
	Door door;
	ASSERT_TRUE(host.Reference(&door, SpatialScope{{72, 20}}));
	const auto id = door.GetNetworkID();
	auto cameraCopy = [&]
	{ return static_cast<Door *>(camera->GetReplication().FindReplica(id)); };
	auto roomCopy = [&]
	{ return static_cast<Door *>(room->GetReplication().FindReplica(id)); };
	ASSERT_TRUE(net.Connect(camera, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cameraCopy() != nullptr; }, 5000));
	ASSERT_TRUE(net.Connect(room, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return roomCopy() != nullptr; }, 5000));
	ASSERT_TRUE(grid.SetPosition(id, {79, 20}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cameraCopy() == nullptr; }, 5000));
	door.state.open = true;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return roomCopy() && roomCopy()->state.open; }, 5000));
	EXPECT_EQ(cameraCopy(), nullptr);
	ASSERT_TRUE(grid.SetPosition(id, {72, 20}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cameraCopy() && cameraCopy()->state.open; }, 5000));
	grid.SetObserverWorld(cameraGuid, 1, 0);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return cameraCopy() == nullptr; }, 5000));
	EXPECT_NE(roomCopy(), nullptr);
	ASSERT_TRUE(host.Destroy(&door));
	EXPECT_FALSE(grid.Contains(id));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return roomCopy() == nullptr; }, 5000));
}

TEST(BuiltinReplication, ManualTickBudgetsConstructionAndCancelsPendingEntries)
{
	SimulatedNetwork net(56);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	auto &service = server->GetReplication();
	auto &remote = client->GetReplication();
	ASSERT_TRUE(service.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(remote.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(service.RegisterReplica<Door>(doorType, 1));
	ASSERT_TRUE(remote.RegisterReplica<Door>(doorType, 1));
	service.SetTickMode(ReplicationTickMode::Manual);
	const auto observer = ToPeerGuid(client->GetMyGUID());
	auto &grid = service.GetInterestGrid();
	ASSERT_TRUE(grid.Configure({{0, 0}, {100, 100}, 10}));
	ASSERT_TRUE(grid.SetObserverCells(observer, {{0, 0}}));
	grid.SetObserverLimits(observer, 0, 1);
	Door first, second;
	ASSERT_TRUE(service.Reference(&first, SpatialScope{{2, 3}, 0, 0, 5}));
	ASSERT_TRUE(service.Reference(&second, SpatialScope{{2, 3}}));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote.GetPeerStatus(ToPeerGuid(server->GetMyGUID())) == ReplicationPeerStatus::Active; }, 5000));
	EXPECT_EQ(remote.GetReplicaCount(), 0u);
	service.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote.FindReplica(first.GetNetworkID()) != nullptr; }, 5000));
	EXPECT_EQ(remote.GetReplicaCount(), 1u);
	ASSERT_TRUE(grid.SetPosition(second.GetNetworkID(), {50, 50}));
	ASSERT_TRUE(grid.SetPosition(first.GetNetworkID(), {50, 50}));
	service.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote.GetReplicaCount() == 0; }, 5000));
}
