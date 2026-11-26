// Copyright (c) 2026, MafiaHub. MIT license.
// The delegation tests' fixture: a server and three clients on the simulated network, and a delegated seat.
#pragma once

#include <gtest/gtest.h>

#include "mafianet/replication_manager.h"
#include "mafianet/snapshot_replica.h"
#include "SimulatedNetwork.h"
#include "../../Source/src/replication_wire.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace DelegationTest
{
using namespace MafiaNet;

constexpr ReplicaTypeId kSeatType{41};
constexpr ReplicaTypeId kSignType{42};

struct SeatState
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct SeatInput
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct Seat : SnapshotReplica<SeatState>
{
	bool Apply(PeerGuid, const SeatInput &input)
	{
		state.value = input.value;
		return true;
	}
};
// Registered without an owner report.
struct Sign : SnapshotReplica<SeatState>
{
};

struct Change
{
	NetworkID id;
	PeerGuid previous, current;
	DelegationChange change;
};

constexpr SpatialPoint kCenter{500, 500};
inline SpatialPoint East(float distance)
{
	return {kCenter.x + distance, kCenter.y};
}

// Remembers the session header of the last decline it sent, so a test can forge malformed ones.
class RecordingPeer final : public RakPeer
{
public:
	using RakPeer::Send;
	ReplicationWire::Header decline{};
	uint32_t Send(const BitStream *data, Priority priority, Reliability reliability, char channel, const AddressOrGUID destination, bool broadcast, uint32_t receipt) override
	{
		BitStream input(data->GetData(), data->GetNumberOfBytesUsed(), false);
		ReplicationWire::Header header{};
		if (ReplicationWire::ReadHeader(input, header) && header.kind == ReplicationWire::Kind::Decline)
		{
			decline = header;
		}
		return RakPeer::Send(data, priority, reliability, channel, destination, broadcast, receipt);
	}
};

// A server and three clients; each client is a radius observer the server places on the grid.
class Delegation : public ::testing::Test
{
protected:
	static constexpr size_t kClients = 3;
	SimulatedNetwork net{57};
	RakPeer *server = net.AddPeer(kClients, kClients);
	std::array<RakPeer *, kClients> clients{AddClient(), AddClient(), AddClient()};
	std::array<PeerGuid, kClients> guids{};
	Seat seat;
	std::vector<Change> changes;

	RakPeer *AddClient() { return net.AddCustomPeer(std::make_unique<RecordingPeer>(), 1); }
	ReplicationManager &Host() { return server->GetReplication(); }
	InterestGrid &Grid() { return Host().GetInterestGrid(); }
	Seat *Copy(size_t client, Seat &original)
	{
		return static_cast<Seat *>(clients[client]->GetReplication().FindReplica(original.GetNetworkID()));
	}
	bool Holds(size_t client, Seat &original)
	{
		auto *copy = Copy(client, original);
		return copy && clients[client]->GetReplication().HasLocalAuthority(*copy);
	}
	void Place(size_t client, SpatialPoint position)
	{
		ASSERT_TRUE(Grid().SetObserverRadius(guids[client], position, 200, 250));
	}
	void Run(uint32_t ms)
	{
		net.RunUntil([]
		             { return false; }, ms);
	}
	// Steps until a change matching the predicate arrives, or the deadline passes.
	bool AwaitChange(const std::function<bool(const Change &)> &match, uint32_t ms = 3000)
	{
		const size_t seen = changes.size();
		return net.RunUntil([&]
		                    {
			for (size_t i = seen; i < changes.size(); ++i)
				if (match(changes[i]))
					return true;
			return false; }, ms, 1);
	}

	void SetUp() override
	{
		ASSERT_TRUE(server->GetReplication().SetRole(ReplicationRole::Server));
		for (auto *client : clients)
			ASSERT_TRUE(client->GetReplication().SetRole(ReplicationRole::Client));
		for (auto *peer : {server, clients[0], clients[1], clients[2]})
		{
			auto &replication = peer->GetReplication();
			ASSERT_TRUE(replication.RegisterReplica<Seat>(kSeatType, 1));
			ASSERT_TRUE(replication.RegisterReplica<Sign>(kSignType, 1));
			ASSERT_TRUE((replication.RegisterOwnerUpdate<Seat, SeatInput>(1, &Seat::Apply)));
		}
		ASSERT_TRUE(Grid().Configure({{0, 0}, {1000, 1000}, 50}));
		ASSERT_TRUE(Host().Reference(&seat, SpatialScope{kCenter}));
		for (size_t i = 0; i < kClients; ++i)
		{
			guids[i] = ToPeerGuid(clients[i]->GetMyGUID());
			ASSERT_TRUE(net.Connect(clients[i], server));
		}
		ASSERT_TRUE(net.RunUntil([&]
		                         {
			for (auto guid : guids)
				if (Host().GetPeerStatus(guid) != ReplicationPeerStatus::Active)
					return false;
			return true; }, 5000));
		// Everyone starts far away (900 from the seat).
		for (size_t i = 0; i < kClients; ++i)
			Place(i, {100, 100 + 100.0f * static_cast<float>(i)});
		Host().SetDelegationCallback([this](NetworkID id, PeerGuid previous, PeerGuid current, DelegationChange change)
		                             { changes.push_back({id, previous, current, change}); });
	}
	void Delegate(Seat &replica, uint32_t weight = 1, uint32_t graceMs = 5000)
	{
		DelegationPolicy policy;
		policy.weight = weight;
		policy.graceMs = graceMs;
		ASSERT_TRUE(Host().SetDelegation(replica, &policy));
	}
	// The client declines (or withdraws) its copy of the seat; waits until the server has counted it.
	void Decline(size_t client, bool declined)
	{
		ASSERT_TRUE(net.RunUntil([&]
		                         { return Copy(client, seat) != nullptr; }, 3000));
		const uint64_t before = Host().GetDelegationStatistics().declines;
		ASSERT_TRUE(clients[client]->GetReplication().DeclineAuthority(*Copy(client, seat), declined));
		if (declined)
		{
			ASSERT_TRUE(net.RunUntil([&]
			                         { return Host().GetDelegationStatistics().declines == before + 1; }, 3000));
		}
		else
		{
			Run(100);
		}
	}
	// The seat is delegated while the client sees it but is beyond acquire range, and the client declines it.
	void DeclineFromAfar(size_t client)
	{
		Place(client, East(150));
		Delegate(seat);
		Decline(client, true);
	}
	// Client 0 near the seat, elected and holding it.
	void AcquireBy(size_t client)
	{
		Place(client, East(50));
		Delegate(seat);
		ASSERT_TRUE(AwaitChange([&](const Change &c)
		                        { return c.change == DelegationChange::Acquired && c.current == guids[client]; }));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return Holds(client, seat); }, 3000));
	}
};
} // namespace DelegationTest
