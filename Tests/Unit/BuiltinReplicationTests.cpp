// Copyright (c) 2026, MafiaHub. MIT license.

#include <gtest/gtest.h>

#include "mafianet/peer_handle.h"
#include "mafianet/message_identifiers.h"
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
	const auto stats = service.GetStatistics(observer);
	EXPECT_EQ(stats.constructed, 1u);
	EXPECT_EQ(stats.deferred, 1u);
	EXPECT_EQ(stats.enters, 1u);
	EXPECT_GT(stats.lifecycleBytes, 0u);
	ASSERT_TRUE(grid.SetPosition(second.GetNetworkID(), {50, 50}));
	ASSERT_TRUE(grid.SetPosition(first.GetNetworkID(), {50, 50}));
	service.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote.GetReplicaCount() == 0; }, 5000));
}

namespace
{
struct SwitchReport
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct ControlledDoor : Door
{
	unsigned accepted = 0;
	bool ApplyReport(PeerGuid, const SwitchReport &report)
	{
		if (report.value > 1)
			return false;
		++accepted;
		state.open = report.value == 1;
		return true;
	}
};
}

TEST(BuiltinReplication, AuthorityTransferRejectsReportsFromAnEarlierGrant)
{
	SimulatedNetwork net(57);
	auto *server = net.AddPeer(2, 2), *alice = net.AddPeer(1), *bob = net.AddPeer(1);
	const auto a = ToPeerGuid(alice->GetMyGUID()), b = ToPeerGuid(bob->GetMyGUID());
	auto &host = server->GetReplication();
	for (auto *p : {server, alice, bob})
	{
		auto &r = p->GetReplication();
		ASSERT_TRUE(r.SetRole(p == server ? ReplicationRole::Server : ReplicationRole::Client));
		ASSERT_TRUE(r.RegisterReplica<ControlledDoor>(doorType, 1));
		ASSERT_TRUE((r.RegisterOwnerUpdate<ControlledDoor, SwitchReport>(1, &ControlledDoor::ApplyReport)));
	}
	auto &grid = host.GetInterestGrid();
	ASSERT_TRUE(grid.Configure({{0, 0}, {100, 100}, 10}));
	ControlledDoor door;
	ASSERT_TRUE(host.Reference(&door, SpatialScope{{90, 90}}));
	const auto id = door.GetNetworkID();
	auto copy = [&](RakPeer *p)
	{ return static_cast<ControlledDoor *>(p->GetReplication().FindReplica(id)); };
	auto owns = [&](RakPeer *p)
	{ return copy(p) && p->GetReplication().HasLocalAuthority(*copy(p)); };
	ASSERT_TRUE(net.Connect(alice, server));
	ASSERT_TRUE(net.Connect(bob, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return host.GetPeerStatus(a) == ReplicationPeerStatus::Active && host.GetPeerStatus(b) == ReplicationPeerStatus::Active; }, 5000));
	EXPECT_EQ(copy(alice), nullptr);
	ASSERT_TRUE(host.SetAuthority(door, a));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return owns(alice); }, 5000));
	ASSERT_TRUE(alice->GetReplication().SendOwnerUpdate(*copy(alice), SwitchReport{1}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return door.state.open; }, 5000));
	// Queue input, then transfer away and back before any networking runs.
	ASSERT_TRUE(alice->GetReplication().SendOwnerUpdate(*copy(alice), SwitchReport{0}));
	ASSERT_TRUE(host.SetAuthority(door, b));
	ASSERT_TRUE(host.SetAuthority(door, a));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return owns(alice) && copy(alice)->state.open; }, 5000));
	net.RunUntil([]
	             { return false; }, 300);
	EXPECT_EQ(door.accepted, 1u);
	ASSERT_TRUE(host.SetAuthority(door, b));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return owns(bob) && !copy(alice); }, 5000));
	ASSERT_TRUE(bob->GetReplication().SendOwnerUpdate(*copy(bob), SwitchReport{42}));
	ASSERT_TRUE(bob->GetReplication().SendOwnerUpdate(*copy(bob), SwitchReport{0}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !door.state.open; }, 5000));
	EXPECT_EQ(door.accepted, 2u);
	grid.SetObserverWorld(b, 0, 55);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !copy(bob); }, 5000));
	EXPECT_FALSE(host.SetAuthority(door, b));
	EXPECT_TRUE(door.WasReferenced());
}

TEST(BuiltinReplication, DelayedStateFromPreviousCellVisitCannotOverwriteFreshBaseline)
{
	struct Recorder : Door
	{
		BitStream earlier;
		void OnSerializeTransmission(BitStream *packet, Connection_RM3 *, BitSize_t *, Time) override
		{
			if (state.open && packet->GetNumberOfBitsUsed() && !earlier.GetNumberOfBitsUsed())
				earlier.WriteBits(packet->GetData(), packet->GetNumberOfBitsUsed(), false);
		}
	};
	SimulatedNetwork net(62);
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	ASSERT_TRUE(host.RegisterReplica<Recorder>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<Recorder>(doorType, 1));
	auto &grid = host.GetInterestGrid();
	ASSERT_TRUE(grid.Configure({{0, 0}, {100, 100}, 10}));
	ASSERT_TRUE(grid.SetObserverCells(ToPeerGuid(client->GetMyGUID()), {{1, 1}}));
	Recorder door;
	ASSERT_TRUE(host.Reference(&door, SpatialScope{{15, 15}}));
	const auto id = door.GetNetworkID();
	auto copy = [&]
	{ return static_cast<Recorder *>(view.FindReplica(id)); };
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() != nullptr; }, 5000));
	door.state.open = true;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy()->state.open && door.earlier.GetNumberOfBitsUsed(); }, 5000));
	ASSERT_TRUE(grid.SetPosition(id, {80, 80}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !copy(); }, 5000));
	door.state.open = false;
	ASSERT_TRUE(grid.SetPosition(id, {15, 15}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() != nullptr; }, 5000));
	ASSERT_FALSE(copy()->state.open);
	host.SetTickMode(ReplicationTickMode::Manual);
	net.Step(300);
	ASSERT_TRUE(server->Send(&door.earlier, Priority::High, Reliability::ReliableOrdered, 0, client->GetMyGUID(), false));
	BitStream marker;
	marker.Write(static_cast<MessageID>(ID_USER_PACKET_ENUM));
	ASSERT_TRUE(server->Send(&marker, Priority::High, Reliability::ReliableOrdered, 0, client->GetMyGUID(), false));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_USER_PACKET_ENUM, 5000));
	EXPECT_FALSE(copy()->state.open);
}

TEST(BuiltinReplication, MarkedSnapshotsConvergeAfterFinalDatagramLoss)
{
	SimulatedNetwork net(91);
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	for (auto *p : {server, client})
	{
		auto &r = p->GetReplication();
		r.SetRole(p == server ? ReplicationRole::Server : ReplicationRole::Client);
		ASSERT_TRUE(r.RegisterReplica<Door>(doorType, 1));
		ASSERT_TRUE(r.SetSnapshotPolicy(doorType, ChangeDetection::Explicit, SnapshotDelivery::Replaceable));
	}
	Door door;
	ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	auto copy = [&]
	{ return static_cast<Door *>(view.FindReplica(door.GetNetworkID())); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() != nullptr; }, 5000));
	door.state.open = true;
	net.Step(300);
	EXPECT_FALSE(copy()->state.open);
	SimulatedNetwork::LinkRule loss;
	loss.dark = true;
	net.SetLink(server, client, loss);
	ASSERT_TRUE(host.MarkDirty(door));
	net.Step(100);
	EXPECT_FALSE(copy()->state.open);
	net.SetLink(server, client, {});
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy()->state.open; }, 5000));
}

TEST(BuiltinReplication, ShutdownInsideTickReleasesBorrowedObjectsAndAllowsRestart)
{
	SimulatedNetwork net(65);
	auto *server = net.AddPeer(1, 1);
	auto &service = server->GetReplication();
	ASSERT_TRUE(service.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(service.RegisterReplica<Door>(doorType, 1));
	Door door;
	ASSERT_TRUE(service.Reference(&door));
	bool after = false;
	service.SetTickCallbacks([&]
	                         { server->Shutdown(0); }, [&]
	                         { after = true; });
	service.Tick();
	EXPECT_FALSE(door.WasReferenced());
	EXPECT_EQ(service.GetConnectionCount(), 0u);
	EXPECT_FALSE(after);
}
