// Copyright (c) 2026, MafiaHub. MIT license.

#include <gtest/gtest.h>

#include "mafianet/peer_handle.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/replication_manager.h"
#include "mafianet/replica_manager3.h"

#include "mafianet/snapshot_replica.h"
#include "SimulatedNetwork.h"
#include "../../Source/src/replication_wire.h"

#include <algorithm>
#include <array>
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
	std::vector<ReplicaEventKind> events;
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
		if (peer == one)
		{
			source.SetNetworkID(0);
			ASSERT_TRUE(service.Reference(&source));
		}
		else
		{
			ASSERT_TRUE(service.ReferenceStatic(&sink, 0));
		}
	}
	two->GetReplication().SetReplicaCallback([&](const ReplicaEvent &e)
	                                         { events.push_back(e.kind); });
	ASSERT_TRUE(net.Connect(two, one));
	EXPECT_TRUE(net.RunUntil([&]
	                         { return sink.state.open; }, 5000));
	EXPECT_EQ(two->GetReplication().FindReplica(0), &sink);
	ASSERT_TRUE(one->GetReplication().Destroy(&source));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return events.size() == 2; }, 5000));
	EXPECT_EQ(events, (std::vector<ReplicaEventKind>{ReplicaEventKind::EnteredScope, ReplicaEventKind::Destroyed}));
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

namespace
{
class RefusingPeer final : public RakPeer
{
public:
	using RakPeer::Send;
	std::set<ReplicationWire::Kind> refuseOnce;
	bool refuseState = false;
	std::function<void(const ReplicationWire::Header &, const BitStream &, PRO, PeerGuid)> inspect;
	bool delayGrant = false, otherVersion = false;
	BitStream delayedGrant;
	uint32_t Send(const BitStream *data, Priority priority, Reliability reliability, char channel, const AddressOrGUID destination, bool broadcast, uint32_t receipt) override
	{
		BitStream input(data->GetData(), data->GetNumberOfBytesUsed(), false);
		ReplicationWire::Header header;
		if (ReplicationWire::ReadHeader(input, header))
		{
			if (inspect)
				inspect(header, *data, {priority, reliability, channel, receipt}, ToPeerGuid(destination.rakNetGuid));
			if (refuseOnce.erase(header.kind) || (refuseState && header.kind == ReplicationWire::Kind::State))
				return 0;
			if (delayGrant && header.kind == ReplicationWire::Kind::Grant)
			{
				delayedGrant.WriteBits(data->GetData(), data->GetNumberOfBitsUsed(), false);
				return 1;
			}
			if (otherVersion && (header.kind == ReplicationWire::Kind::Hello || header.kind == ReplicationWire::Kind::Reject))
			{
				BitStream translated(data->GetData(), data->GetNumberOfBytesUsed(), true);
				translated.SetWriteOffset(data->GetNumberOfBitsUsed());
				translated.GetData()[1] = ReplicationWire::Version + 1;
				return RakPeer::Send(&translated, priority, reliability, channel, destination, broadcast, receipt);
			}
		}
		return RakPeer::Send(data, priority, reliability, channel, destination, broadcast, receipt);
	}
};
}

TEST(BuiltinReplication, ControlAndRemovalSurviveLocalEnqueueRefusal)
{
	using Kind = ReplicationWire::Kind;
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1));
	auto &host = server->GetReplication();
	auto &remote = client->GetReplication();
	for (auto *p : {server, client})
	{
		auto &r = p->GetReplication();
		ASSERT_TRUE(r.SetRole(p == server ? ReplicationRole::Server : ReplicationRole::Client));
		ASSERT_TRUE(r.RegisterReplica<ControlledDoor>(doorType, 1));
		ASSERT_TRUE((r.RegisterOwnerUpdate<ControlledDoor, SwitchReport>(1, &ControlledDoor::ApplyReport)));
	}
	client->refuseOnce = {Kind::Hello, Kind::OwnerUpdate};
	server->refuseOnce = {Kind::Accept, Kind::Grant, Kind::Revoke, Kind::Destroy};
	ControlledDoor door;
	ASSERT_TRUE(host.Reference(&door));
	const auto id = door.GetNetworkID();
	auto copy = [&]
	{ return static_cast<ControlledDoor *>(remote.FindReplica(id)); };
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy(); }, 5000));
	ASSERT_TRUE(host.SetAuthority(door, ToPeerGuid(client->GetMyGUID())));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return remote.HasLocalAuthority(*copy()); }, 5000));
	EXPECT_FALSE(remote.SendOwnerUpdate(*copy(), SwitchReport{1}));
	EXPECT_TRUE(remote.SendOwnerUpdate(*copy(), SwitchReport{1}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return door.accepted == 1; }, 5000));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy()->state.open; }, 5000));
	server->refuseOnce.insert(Kind::State);
	door.state.open = false;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !copy()->state.open; }, 5000));
	// A correction can be revoked before its replacement grant was published.
	ASSERT_TRUE(host.CorrectAuthority(door));
	ASSERT_TRUE(host.RevokeAuthority(door));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !remote.HasLocalAuthority(*copy()); }, 5000));
	ASSERT_TRUE(host.Destroy(&door));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !copy(); }, 5000));
	EXPECT_TRUE(server->refuseOnce.empty());
	EXPECT_TRUE(client->refuseOnce.empty());
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
	grid.SetObserverWorld(b, 0, 0);
	ASSERT_TRUE(host.SetAuthority(door, b));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return owns(bob); }, 5000));
	bob->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return host.GetConnectionCount() == 1 && !copy(bob); }, 5000));
	EXPECT_FALSE(host.SetAuthority(door, b));
	ASSERT_TRUE(host.SetAuthority(door, a));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return owns(alice); }, 5000));
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
	service.SetTickCallbacks({}, {});
	SocketDescriptor socket(0, "10.0.0.1");
	ASSERT_EQ(server->Startup(1, &socket, 1), RAKNET_STARTED);
	ASSERT_TRUE(service.Reference(&door));
	service.Tick();
	EXPECT_TRUE(door.WasReferenced());
}

TEST(BuiltinReplication, AdmissionAndRemotePoolOwnThePeerCreatedCopy)
{
	unsigned allocated = 0, released = 0;
	SimulatedNetwork net(104);
	auto *publisher = net.AddPeer(1, 1), *subscriber = net.AddPeer(1);
	auto &source = publisher->GetReplication(), &sink = subscriber->GetReplication();
	source.SetRole(ReplicationRole::Peer);
	sink.SetRole(ReplicationRole::Peer);
	source.SetAutomaticAdmission(false);
	ASSERT_TRUE(source.RegisterReplica<Door>(doorType, 1));
	ASSERT_TRUE(sink.RegisterReplica<Door>(doorType, 1, [&]
	                                       {++allocated;return new Door; }, [&](Door *door)
	                                       {++released;delete door; }));
	Door door;
	ASSERT_TRUE(source.Reference(&door));
	ASSERT_TRUE(net.Connect(subscriber, publisher));
	net.Step(100);
	ASSERT_EQ(allocated, 0u);
	ASSERT_TRUE(source.Admit(ToPeerGuid(subscriber->GetMyGUID())));
	auto copy = [&]
	{ return sink.FindReplica(door.GetNetworkID()); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() != nullptr; }, 5000));
	EXPECT_FALSE(sink.Destroy(copy()));
	EXPECT_EQ(allocated, 1u);
	ASSERT_TRUE(source.Destroy(&door));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return released == 1; }, 5000));
	EXPECT_EQ(copy(), nullptr);
}

TEST(BuiltinReplication, GrantedBaselinePrecedesTheOrdinaryConstructionBacklog)
{
	SimulatedNetwork net(114);
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	for (auto *peer : {server, client})
	{
		auto &r = peer->GetReplication();
		r.SetRole(peer == server ? ReplicationRole::Server : ReplicationRole::Client);
		ASSERT_TRUE(r.RegisterReplica<ControlledDoor>(doorType, 1));
		ASSERT_TRUE((r.RegisterOwnerUpdate<ControlledDoor, SwitchReport>(1, &ControlledDoor::ApplyReport)));
	}
	const auto guid = ToPeerGuid(client->GetMyGUID());
	host.SetTickMode(ReplicationTickMode::Manual);
	auto &grid = host.GetInterestGrid();
	ASSERT_TRUE(grid.Configure({{0, 0}, {100, 100}, 10}));
	ASSERT_TRUE(grid.SetObserverCells(guid, {{0, 0}}));
	grid.SetObserverLimits(guid, 0, 1);
	ControlledDoor nearby[3], controlled;
	for (auto &door : nearby)
		ASSERT_TRUE(host.Reference(&door, SpatialScope{{5, 5}}));
	ASSERT_TRUE(host.Reference(&controlled, SpatialScope{{95, 95}}));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetPeerStatus(ToPeerGuid(server->GetMyGUID())) == ReplicationPeerStatus::Active; }, 5000));
	host.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetReplicaCount() == 1; }, 5000));
	ASSERT_TRUE(host.SetAuthority(controlled, guid));
	host.Tick();
	auto copy = [&]
	{ return static_cast<ControlledDoor *>(view.FindReplica(controlled.GetNetworkID())); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy() && view.HasLocalAuthority(*copy()); }, 5000));
	ASSERT_TRUE(view.SendOwnerUpdate(*copy(), SwitchReport{1}));
	ASSERT_TRUE(host.CorrectAuthority(controlled));
	host.Tick();
	net.Step(300);
	EXPECT_EQ(controlled.accepted, 0u);
	EXPECT_FALSE(copy()->state.open);
	EXPECT_TRUE(host.RevokeAuthority(controlled));
	host.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !copy(); }, 5000));
}

TEST(BuiltinReplication, OversizedConstructionRetriesWithTheCurrentSnapshot)
{
	struct LargeDoor : Door
	{
		bool oversize = true;
		void SerializeConstruction(BitStream *out, Connection_RM3 *connection) override
		{
			if (oversize)
			{
				const std::vector<unsigned char> data(1024 * 1024 + 1, 0);
				out->WriteAlignedBytes(data.data(), static_cast<unsigned int>(data.size()));
			}
			else
				Door::SerializeConstruction(out, connection);
		}
	};
	SimulatedNetwork net(117);
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	host.SetTickMode(ReplicationTickMode::Manual);
	ASSERT_TRUE(host.RegisterReplica<LargeDoor>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<LargeDoor>(doorType, 1));
	LargeDoor door;
	ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return host.GetConnectionCount() == 1 && view.GetConnectionCount() == 1; }, 5000));
	host.Tick();
	door.oversize = false;
	door.state.open = true;
	host.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.FindReplica(door.GetNetworkID()) != nullptr; }, 5000));
	EXPECT_TRUE(static_cast<LargeDoor *>(view.FindReplica(door.GetNetworkID()))->state.open);
}

TEST(BuiltinReplication, ApplicationFilterSeesNativeEnvelopesBeforeTheService)
{
	struct Gate : PluginInterface2
	{
		unsigned envelopes = 0;
		PluginReceiveResult OnReceive(Packet *packet) override
		{
			if (packet->length && packet->data[0] == ID_REPLICATION)
			{
				++envelopes;
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			}
			return RR_CONTINUE_PROCESSING;
		}
	} gate;
	SimulatedNetwork net(118);
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	server->AttachPlugin(&gate);
	server->GetReplication().SetRole(ReplicationRole::Server);
	client->GetReplication().SetRole(ReplicationRole::Client);
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return gate.envelopes != 0; }, 5000));
	EXPECT_EQ(server->GetReplication().GetConnectionCount(), 0u);
	EXPECT_FALSE(net.Received(server, ID_REPLICATION));
	server->DetachPlugin(&gate);
}

TEST(BuiltinReplication, LifecycleEventsDescribeCommittedStateAcrossReentryTransferAndRelease)
{
	using E = ReplicaEventKind;
	std::vector<E> seen;
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &remote = client->GetReplication();
	for (auto *p : {server, client})
	{
		auto &r = p->GetReplication();
		ASSERT_TRUE(r.SetRole(p == server ? ReplicationRole::Server : ReplicationRole::Client));
		ASSERT_TRUE(r.RegisterReplica<ControlledDoor>(doorType, 1));
		ASSERT_TRUE((r.RegisterOwnerUpdate<ControlledDoor, SwitchReport>(1, &ControlledDoor::ApplyReport)));
	}
	ControlledDoor door;
	auto &grid = host.GetInterestGrid();
	const auto guid = ToPeerGuid(client->GetMyGUID());
	ASSERT_TRUE(grid.Configure({{0, 0}, {20, 10}, 10}));
	ASSERT_TRUE(grid.SetObserverCells(guid, {{0, 0}}));
	ASSERT_TRUE(host.Reference(&door, SpatialScope{{2, 2}}));
	const auto id = door.GetNetworkID();
	bool releaseOnEnter = false;
	remote.SetReplicaCallback([&, count = size_t{0}](const ReplicaEvent &event) mutable
	                          {
		EXPECT_EQ(++count, seen.size() + 1);
		seen.push_back(event.kind);
		EXPECT_EQ(event.id, id);
		EXPECT_EQ(event.source, ToPeerGuid(server->GetMyGUID()));
		if (event.kind == E::EnteredScope)
		{
			EXPECT_NE(remote.FindReplica(id), nullptr);
			if (releaseOnEnter)
			{
				EXPECT_TRUE(remote.ReleaseRemote(id));
			}
		}
		else if (event.kind == E::Released || event.kind == E::LeftScope)
		{
			EXPECT_EQ(remote.FindReplica(id), nullptr);
		} });
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !seen.empty(); }, 5000));
	ASSERT_TRUE(host.SetAuthority(door, guid));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return seen.back() == E::AuthorityGranted; }, 5000));
	ASSERT_TRUE(host.CorrectAuthority(door));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return seen.back() == E::AuthorityCorrected; }, 5000));
	ASSERT_TRUE(host.RevokeAuthority(door));
	ASSERT_TRUE(grid.SetPosition(id, {12, 2}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return seen.back() == E::LeftScope; }, 5000));
	releaseOnEnter = true;
	ASSERT_TRUE(grid.SetPosition(id, {2, 2}));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return seen.back() == E::Released; }, 5000));
	EXPECT_EQ(seen, (std::vector<E>{E::EnteredScope, E::AuthorityGranted, E::AuthorityCorrected, E::AuthorityLost, E::LeftScope, E::EnteredScope, E::Released}));
	remote.SetReplicaCallback({});
}

TEST(BuiltinReplication, FailedDecodePublishesNoVisitAndDisconnectReleasesOnlyCommittedCopies)
{
	struct CheckedDoor : Door
	{
		bool valid = true;
		void SerializeConstruction(BitStream *out, Connection_RM3 *connection) override
		{
			if (valid)
				Door::SerializeConstruction(out, connection);
		}
	};
	using E = ReplicaEventKind;
	std::vector<ReplicaEvent> seen;
	unsigned released = 0;
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &remote = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(remote.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(host.RegisterReplica<CheckedDoor>(doorType, 1));
	ASSERT_TRUE(remote.RegisterReplica<CheckedDoor>(doorType, 1, []
	                                                { return new CheckedDoor; }, [&](CheckedDoor *door)
	                                                {++released; delete door; }));
	CheckedDoor good, bad;
	bad.valid = false;
	ASSERT_TRUE(host.Reference(&good));
	ASSERT_TRUE(host.Reference(&bad));
	remote.SetReplicaCallback([&](const ReplicaEvent &event)
	                          {
		seen.push_back(event);
		if(event.kind==E::Disconnected)
		{
			EXPECT_EQ(released,2u);
			EXPECT_EQ(remote.FindReplica(event.id),nullptr);
		} });
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return released == 1 && seen.size() == 1; }, 5000));
	ASSERT_EQ(seen[0].kind, E::EnteredScope);
	EXPECT_EQ(seen[0].id, good.GetNetworkID());
	client->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return seen.size() == 2; }, 5000));
	EXPECT_EQ(seen[1].kind, E::Disconnected);
	EXPECT_EQ(seen[1].id, good.GetNetworkID());
}

TEST(BuiltinReplication, BlockedConnectionResumesWithLatestExplicitState)
{
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &remote = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	remote.SetRole(ReplicationRole::Client);
	for (auto *r : {&host, &remote})
	{
		ASSERT_TRUE(r->RegisterReplica<Door>(doorType, 1));
		ASSERT_TRUE(r->SetSnapshotPolicy(doorType, ChangeDetection::Explicit, SnapshotDelivery::Reliable));
	}
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	Door door;
	ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return host.GetConnectionCount() == 1; }, 5000));
	host.Tick();
	auto copy = [&]
	{ return static_cast<Door *>(remote.FindReplica(door.GetNetworkID())); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy(); }, 5000));
	unsigned passes = 0;
	host.SetSerializationList([&](PeerGuid, std::vector<Replica3 *> &)
	                          {++passes;return false; });
	server->refuseState = true;
	door.state.open = true;
	ASSERT_TRUE(host.MarkDirty(door));
	host.Tick();
	const auto blockedAt = passes;
	ASSERT_GT(blockedAt, 0u);
	door.state.open = false;
	ASSERT_TRUE(host.MarkDirty(door));
	for (unsigned i = 0; i < 10; ++i)
		host.Tick();
	EXPECT_EQ(passes, blockedAt);
	server->refuseState = false;
	host.Tick();
	EXPECT_GT(passes, blockedAt);
	// An extra tick of network progress delivers both the queued and superseding state.
	net.Step(100);
	EXPECT_FALSE(copy()->state.open);
	door.state.open = true;
	ASSERT_TRUE(host.MarkDirty(door));
	host.Tick();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy()->state.open; }, 5000));
}

TEST(BuiltinReplication, AGrantOvertakenByANewerSnapshotPreservesTheNewState)
{
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	for (auto *r : {&host, &view})
	{
		ASSERT_TRUE(r->RegisterReplica<ControlledDoor>(doorType, 1));
		ASSERT_TRUE((r->RegisterOwnerUpdate<ControlledDoor, SwitchReport>(1, &ControlledDoor::ApplyReport)));
		ASSERT_TRUE(r->SetSnapshotPolicy(doorType, ChangeDetection::Explicit, SnapshotDelivery::Replaceable));
	}
	host.SetSerializeInterval(0);
	ControlledDoor door;
	ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	auto copy = [&]
	{ return static_cast<ControlledDoor *>(view.FindReplica(door.GetNetworkID())); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy(); }, 5000));
	server->delayGrant = true;
	ASSERT_TRUE(host.SetAuthority(door, ToPeerGuid(client->GetMyGUID())));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return server->delayedGrant.GetNumberOfBitsUsed() != 0; }, 5000));
	door.state.open = true;
	ASSERT_TRUE(host.MarkDirty(door));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy()->state.open; }, 5000));
	server->delayGrant = false;
	ASSERT_TRUE(server->RakPeer::Send(&server->delayedGrant, Priority::High, Reliability::ReliableOrdered, 0, client->GetMyGUID(), false));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.HasLocalAuthority(*copy()); }, 5000));
	EXPECT_TRUE(copy()->state.open);
}

TEST(BuiltinReplication, DifferentEnvelopeVersionReportsMismatchOnBothPeers)
{
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1));
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	server->otherVersion = client->otherVersion = true;
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetPeerStatus(ToPeerGuid(server->GetMyGUID())) == ReplicationPeerStatus::ProtocolMismatch; }, 5000));
	EXPECT_EQ(host.GetPeerStatus(ToPeerGuid(client->GetMyGUID())), ReplicationPeerStatus::ProtocolMismatch);
}

TEST(BuiltinReplication, PreexistingObjectCanSkipItsBaselineAndStillReceiveState)
{
	struct Existing : Door
	{
		unsigned baselines = 0;
		RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_ALREADY_EXISTS_REMOTELY_DO_NOT_CONSTRUCT; }
		void DeserializeConstructionExisting(BitStream *, Connection_RM3 *) override { ++baselines; }
	};
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	ASSERT_TRUE(host.RegisterReplica<Existing>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<Existing>(doorType, 1));
	Existing source, sink;
	ASSERT_TRUE(host.Reference(&source));
	ASSERT_TRUE(view.ReferenceStatic(&sink, source.GetNetworkID()));
	ASSERT_TRUE(net.Connect(client, server));
	source.state.open = true;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return sink.state.open; }, 5000));
	EXPECT_EQ(sink.baselines, 0u);
}

TEST(BuiltinReplication, OversizedStateIsReportedBeforeUpdatingTheSendCache)
{
	struct LargeState : Door
	{
		bool oversize = false;
		RM3SerializationResult Serialize(SerializeParameters *params) override
		{
			if (oversize)
			{
				std::vector<unsigned char> bytes(1024 * 1024 + 1);
				params->outputBitstream[0].WriteAlignedBytes(bytes.data(), static_cast<unsigned>(bytes.size()));
				return RM3SR_BROADCAST_IDENTICALLY;
			}
			return Door::Serialize(params);
		}
	};
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	ASSERT_TRUE(host.RegisterReplica<LargeState>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<LargeState>(doorType, 1));
	LargeState source;
	ASSERT_TRUE(host.Reference(&source));
	ASSERT_TRUE(net.Connect(client, server));
	auto copy = [&]
	{ return static_cast<Door *>(view.FindReplica(source.GetNetworkID())); };
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy(); }, 5000));
	source.state.open = source.oversize = true;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return source.GetRejectedStateCount() > 0; }, 5000));
	EXPECT_FALSE(copy()->state.open);
	source.oversize = false;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return copy()->state.open; }, 5000));
}

TEST(BuiltinReplication, StateCallbackCannotStartANestedWorldTick)
{
	struct TickDoor : Door
	{
		std::function<void()> attempt;
		void Deserialize(DeserializeParameters *parameters) override
		{
			Door::Deserialize(parameters);
			if (attempt)
				attempt();
		}
	};
	unsigned callbacks = 0, ticks = 0;
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	view.SetTickMode(ReplicationTickMode::Manual);
	view.SetTickCallbacks([&]
	                      { ++ticks; }, {});
	ASSERT_TRUE(host.RegisterReplica<TickDoor>(doorType, 1));
	ASSERT_TRUE(view.RegisterReplica<TickDoor>(doorType, 1, [&]
	                                           {
		auto *door=new TickDoor;
		door->attempt=[&]{++callbacks;view.Tick();};
		return door; }, [](TickDoor *door)
	                                           { delete door; }));
	TickDoor door;
	ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.FindReplica(door.GetNetworkID()); }, 5000));
	door.state.open = true;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return callbacks > 0; }, 5000));
	EXPECT_EQ(ticks, 0u);
	view.Tick();
	EXPECT_EQ(ticks, 1u);
}

TEST(BuiltinReplication, ARefusedBatchRetainsOnlyObjectsStillInScope)
{
	using Kind = ReplicationWire::Kind;
	unsigned batches = 0, retriedStates = 0;
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	for (auto *service : {&host, &view})
	{
		ASSERT_TRUE(service->RegisterReplica<Door>(doorType, 1));
		ASSERT_TRUE(service->SetSnapshotPolicy(doorType, ChangeDetection::Explicit, SnapshotDelivery::Reliable));
	}
	// Two adjacent one-bit updates exercise the sub-byte record boundaries.
	Door first, second;
	ASSERT_TRUE(host.Reference(&first));
	ASSERT_TRUE(host.Reference(&second));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetReplicaCount() == 2; }, 5000));
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	server->inspect = [&](const ReplicationWire::Header &header, const BitStream &, PRO, PeerGuid)
	{
		if (header.kind == Kind::StateBatch)
			++batches;
		if (header.kind == Kind::State)
			++retriedStates;
	};
	server->refuseOnce.insert(Kind::StateBatch);
	first.state.open = second.state.open = true;
	ASSERT_TRUE(host.MarkDirty(first));
	ASSERT_TRUE(host.MarkDirty(second));
	host.Tick();
	ASSERT_EQ(batches, 1u);
	ASSERT_TRUE(host.Destroy(&first));
	host.Tick();
	auto received = [&]
	{
		auto *copy = static_cast<Door *>(view.FindReplica(second.GetNetworkID()));
		return view.GetReplicaCount() == 1 && copy && copy->state.open;
	};
	ASSERT_TRUE(net.RunUntil(received, 5000));
	EXPECT_EQ(retriedStates, 1u);
}

TEST(BuiltinReplication, LostBatchRepairsEveryReplaceableSnapshot)
{
	using Kind = ReplicationWire::Kind;
	unsigned batches = 0, repairs = 0;
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	host.SetRole(ReplicationRole::Server);
	view.SetRole(ReplicationRole::Client);
	for (auto *r : {&host, &view})
	{
		ASSERT_TRUE(r->RegisterReplica<Door>(doorType, 1));
		ASSERT_TRUE(r->SetSnapshotPolicy(doorType, ChangeDetection::Explicit, SnapshotDelivery::Replaceable));
	}
	std::array<Door, 3> doors;
	for (auto &door : doors)
	{
		ASSERT_TRUE(host.Reference(&door));
	}
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetReplicaCount() == doors.size(); }, 5000));
	net.Step(300);
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	server->inspect = [&](const ReplicationWire::Header &header, const BitStream &, PRO pro, PeerGuid)
	{
		if (header.kind == Kind::SnapshotBatch)
		{
			++batches;
			EXPECT_EQ(pro.reliability, Reliability::Unreliable);
		}
		if (header.kind == Kind::Snapshot && pro.reliability == Reliability::ReliableOrdered)
			++repairs;
	};
	SimulatedNetwork::LinkRule loss;
	loss.dark = true;
	net.SetLink(server, client, loss);
	for (auto &door : doors)
	{
		door.state.open = true;
		ASSERT_TRUE(host.MarkDirty(door));
	}
	host.Tick();
	ASSERT_EQ(batches, 1u);
	net.Step(300);
	net.SetLink(server, client, {});
	host.Tick();
	ASSERT_EQ(repairs, doors.size());
	auto openOnClient = [&](Door &door)
	{
		auto *copy = static_cast<Door *>(view.FindReplica(door.GetNetworkID()));
		return copy && copy->state.open;
	};
	ASSERT_TRUE(net.RunUntil([&]
	                         { return std::all_of(doors.begin(), doors.end(), openOnClient); }, 5000));
}

TEST(BuiltinReplication, BatchesAdjacentCompatibleRecordsAndPreservesIndividualSendOptions)
{
	using namespace ReplicationWire;
	struct RoutedDoor : Door
	{
		PRO route{Priority::High, Reliability::ReliableOrdered, 0, 0};
		std::vector<unsigned char> payload;
		bool BroadcastsIdentically() const override { return false; }
		RM3SerializationResult Serialize(SerializeParameters *params) override
		{
			params->pro[0] = route;
			params->outputBitstream[0].Write(state.open);
			if (!payload.empty())
				params->outputBitstream[0].WriteAlignedBytes(payload.data(), static_cast<unsigned>(payload.size()));
			return RM3SR_SERIALIZED_UNIQUELY;
		}
		void Deserialize(DeserializeParameters *params) override
		{
			auto &in = params->serializationBitstream[0];
			ASSERT_TRUE(in.Read(state.open));
			if (in.GetNumberOfUnreadBits())
			{
				in.AlignReadToByteBoundary();
				payload.resize(in.GetNumberOfUnreadBits() / 8);
				ASSERT_TRUE(in.ReadAlignedBytes(payload.data(), static_cast<unsigned>(payload.size())));
			}
		}
	};
	std::vector<Kind> kinds;
	std::vector<PRO> options;
	std::array<RoutedDoor, 13> doors;
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	for (auto *service : {&host, &view})
		ASSERT_TRUE(service->RegisterReplica<RoutedDoor>(doorType, 1));
	for (auto &door : doors)
		ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetReplicaCount() == doors.size(); }, 5000));
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	host.SetSerializationList([&](PeerGuid, std::vector<Replica3 *> &out)
	                          {
		for (auto &door : doors)
			out.push_back(&door);
		return true; });
	for (auto &door : doors)
		door.state.open = true;
	for (unsigned i : {2u, 3u, 4u, 5u})
		doors[i].route.orderingChannel = 1;
	doors[4].route.priority = doors[5].route.priority = Priority::Medium;
	doors[6].route = {Priority::High, Reliability::ReliableOrderedWithAckReceipt, 0, 413};
	doors[7].route = {Priority::High, Reliability::ReliableOrderedWithAckReceipt, 0, 414};
	doors[8].route.reliability = doors[9].route.reliability = Reliability::ReliableSequenced;
	doors[8].route.orderingChannel = doors[9].route.orderingChannel = 2;
	doors[10].route.priority = Priority::Immediate;
	doors[11].payload.assign(2048, 0x3c);
	server->inspect = [&](const Header &header, const BitStream &data, PRO pro, PeerGuid)
	{
		if (header.kind != Kind::State && header.kind != Kind::StateBatch)
			return;
		kinds.push_back(header.kind);
		options.push_back(pro);
		if (header.kind == Kind::StateBatch)
		{
			EXPECT_LE(data.GetNumberOfBytesUsed(), MaxBatchBytes);
		}
	};
	host.Tick();
	const std::vector<Kind> expected{Kind::StateBatch, Kind::StateBatch, Kind::StateBatch, Kind::State, Kind::State, Kind::State, Kind::State, Kind::State, Kind::State, Kind::State};
	EXPECT_EQ(kinds, expected);
	ASSERT_EQ(options.size(), expected.size());
	unsigned index = 0;
	for (unsigned leader : {0u, 2u, 4u, 6u, 7u, 8u, 9u, 10u, 11u, 12u})
		EXPECT_TRUE(options[index++] == doors[leader].route);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		for (auto &door : doors)
		{
			if (door.route.reliability == Reliability::ReliableSequenced)
				continue;
			auto *copy = static_cast<RoutedDoor *>(view.FindReplica(door.GetNetworkID()));
			if (!copy || !copy->state.open || copy->payload != door.payload)
				return false;
		}
		return true; }, 5000));
}

TEST(BuiltinReplication, EachObserversCellUsesItsOwnPathMtu)
{
	using namespace ReplicationWire;
	std::array<Door, 80> doors;
	std::map<PeerGuid, std::set<NetworkID>> expected, received;
	std::map<PeerGuid, unsigned> largestBatch;
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 2, 2));
	std::array<RakPeer *, 2> clients{net.AddPeer(1), net.AddPeer(1)};
	auto &host = server->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(host.RegisterReplica<Door>(doorType, 1));
	ASSERT_TRUE(host.GetInterestGrid().Configure({{0, 0}, {2, 1}, 1}));
	SimulatedNetwork::LinkRule narrow;
	narrow.maxDatagramBytes = 650;
	net.SetLinkBothWays(server, clients[0], narrow);
	for (int cell = 0; cell < 2; ++cell)
	{
		auto *client = clients[cell];
		auto &view = client->GetReplication();
		ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
		ASSERT_TRUE(view.RegisterReplica<Door>(doorType, 1));
		ASSERT_TRUE(net.Connect(client, server, 20000));
		const auto guid = ToPeerGuid(client->GetMyGUID());
		ASSERT_TRUE(host.GetInterestGrid().SetObserverCells(guid, {{cell, 0}}));
		for (unsigned i = static_cast<unsigned>(cell); i < doors.size(); i += 2)
		{
			ASSERT_TRUE(host.Reference(&doors[i], SpatialScope{{static_cast<float>(cell) + 0.5f, 0.5f}}));
			expected[guid].insert(doors[i].GetNetworkID());
		}
	}
	auto ready = [&]
	{ return clients[0]->GetReplication().GetReplicaCount() == 40 && clients[1]->GetReplication().GetReplicaCount() == 40; };
	ASSERT_TRUE(net.RunUntil(ready, 5000));
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	server->inspect = [&](const Header &header, const BitStream &packet, PRO, PeerGuid recipient)
	{
		if (header.kind != Kind::State && header.kind != Kind::StateBatch)
			return;
		BitStream in(packet.GetData(), packet.GetNumberOfBytesUsed(), false);
		in.SetWriteOffset(packet.GetNumberOfBitsUsed());
		in.SetReadOffset(HeaderBytes * 8);
		auto collect = [&](BitStream &body)
		{
			NetworkID id;
			ASSERT_TRUE(body.Read(id));
			EXPECT_TRUE(received[recipient].insert(id).second);
		};
		if (header.kind == Kind::State)
			collect(in);
		else
		{
			largestBatch[recipient] = std::max(largestBatch[recipient], packet.GetNumberOfBytesUsed());
			StateBatchView view;
			ASSERT_TRUE(view.Read(in));
			for (unsigned i = 0; i < view.count; ++i)
			{
				const auto record = view.records[i];
				BitStream body(record.data, BITS_TO_BYTES(record.bits), false);
				collect(body);
			}
		}
	};
	for (auto &door : doors)
		door.state.open = true;
	host.Tick();
	EXPECT_EQ(expected, received);
	const auto narrowPeer = ToPeerGuid(clients[0]->GetMyGUID());
	const auto widePeer = ToPeerGuid(clients[1]->GetMyGUID());
	const int mtu = server->GetMTUSize(net.AddressOf(clients[0]));
	EXPECT_GT(largestBatch[narrowPeer], 0u);
	EXPECT_LE(largestBatch[narrowPeer], static_cast<unsigned>(mtu - 96));
	EXPECT_GT(largestBatch[widePeer], static_cast<unsigned>(mtu));
}

TEST(BuiltinReplication, ClosingTheClientInAStateCallbackEndsBatchDelivery)
{
	struct ClosingDoor : Door
	{
		RakPeer *client = nullptr;
		unsigned *delivered = nullptr;
		void Deserialize(DeserializeParameters *params) override
		{
			Door::Deserialize(params);
			if (state.open)
			{
				++*delivered;
				client->Shutdown(0);
				EXPECT_EQ(client->Receive(), nullptr);
				EXPECT_TRUE(state.open);
			}
		}
	};
	unsigned callbacks = 0, acknowledgments = 0, batches = 0;
	std::array<ClosingDoor, 2> doors;
	SimulatedNetwork net;
	auto *server = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1, 1));
	auto *client = static_cast<RefusingPeer *>(net.AddCustomPeer(std::make_unique<RefusingPeer>(), 1));
	auto &host = server->GetReplication(), &view = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(host.RegisterReplica<ClosingDoor>(doorType, 1));
	auto create = [&]
	{
		auto *door = new ClosingDoor;
		door->client = client;
		door->delivered = &callbacks;
		return door;
	};
	ASSERT_TRUE(view.RegisterReplica<ClosingDoor>(doorType, 1, create, [](ClosingDoor *door)
	                                              { delete door; }));
	for (auto *service : {&host, &view})
		ASSERT_TRUE(service->SetSnapshotPolicy(doorType, ChangeDetection::Explicit, SnapshotDelivery::Replaceable));
	for (auto &door : doors)
		ASSERT_TRUE(host.Reference(&door));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetReplicaCount() == doors.size(); }, 5000));
	net.Step(300);
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	server->inspect = [&](const ReplicationWire::Header &header, const BitStream &, PRO, PeerGuid)
	{
		if (header.kind == ReplicationWire::Kind::SnapshotBatch)
			++batches;
	};
	client->inspect = [&](const ReplicationWire::Header &header, const BitStream &, PRO, PeerGuid)
	{
		if (header.kind == ReplicationWire::Kind::SnapshotAck)
			++acknowledgments;
	};
	for (auto &door : doors)
	{
		door.state.open = true;
		ASSERT_TRUE(host.MarkDirty(door));
	}
	host.Tick();
	ASSERT_EQ(batches, 1u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return callbacks > 0; }, 5000));
	EXPECT_EQ(callbacks, 1u);
	EXPECT_EQ(acknowledgments, 0u);
	EXPECT_EQ(view.GetReplicaCount(), 0u);
}
