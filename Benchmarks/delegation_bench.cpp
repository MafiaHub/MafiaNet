// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/replication_manager.h"
#include "mafianet/snapshot_replica.h"
#include "SimulatedNetwork.h"
#include <benchmark/benchmark.h>
#include <memory>
#include <vector>

using namespace MafiaNet;

namespace
{
struct NpcState
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct NpcInput
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct Npc : SnapshotReplica<NpcState>
{
	bool Apply(PeerGuid, const NpcInput &input)
	{
		state.value = input.value;
		return true;
	}
};
constexpr ReplicaTypeId kNpcType{77};
constexpr unsigned kClients = 800;
constexpr unsigned kNpcs = 2000;
constexpr float kWorld = 4000;
constexpr uint32_t kTickMs = 33;

bool Register(ReplicationManager &replication)
{
	return replication.RegisterReplica<Npc>(kNpcType, 1) && replication.RegisterOwnerUpdate<Npc, NpcInput>(1, &Npc::Apply);
}

// Spreads index i of count over the world on a jittered lattice, deterministic.
SpatialPoint Spread(unsigned i, unsigned count, uint32_t salt)
{
	unsigned side = 1;
	while (side * side < count)
		++side;
	const float cell = kWorld / static_cast<float>(side);
	uint32_t hash = (i + 1) * 2654435761u ^ salt;
	hash ^= hash >> 15;
	const float jx = static_cast<float>(hash & 0xff) / 255.0f, jy = static_cast<float>((hash >> 8) & 0xff) / 255.0f;
	return {(static_cast<float>(i % side) + jx) * cell, (static_cast<float>(i / side) + jy) * cell};
}

// One server Tick with 2,000 delegated replicas spread over a 4,000 x 4,000 world and 800 Active clients that are
// radius observers. The arg sets the election pass: 0 off (an interval so long that nothing is ever due), 1 the
// default 500 ms interval (about 1/15 of the replicas per tick), 2 every replica on every tick. Holders and grants are
// the same in all three, so the difference from pass:0 is the pass's own cost per tick; pass:2 shows the whole pass
// with a signal well above the Tick's noise. Ticks are 33 ms of virtual time apart (30 Hz); the network steps between
// ticks, untimed.
void BM_DelegationPass(benchmark::State &state)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(kClients, kClients);
	auto &host = server->GetReplication();
	if (!host.SetRole(ReplicationRole::Server) || !Register(host))
	{
		state.SkipWithError("server setup failed");
		return;
	}
	// Connected in batches: the server takes a limited number of handshakes at once.
	std::vector<RakPeer *> clients;
	std::vector<PeerGuid> guids;
	constexpr unsigned kBatch = 16;
	for (unsigned first = 0; first < kClients; first += kBatch)
	{
		for (unsigned i = first; i < first + kBatch && i < kClients; ++i)
		{
			RakPeer *client = net.AddPeer(1);
			if (!client->GetReplication().SetRole(ReplicationRole::Client) || !Register(client->GetReplication()) || net.StartConnect(client, server) != CONNECTION_ATTEMPT_STARTED)
			{
				state.SkipWithError("client setup failed");
				return;
			}
			clients.push_back(client);
			guids.push_back(ToPeerGuid(client->GetMyGUID()));
		}
		const bool active = net.RunUntil([&]
		                                 {
			for (unsigned i = first; i < guids.size(); ++i)
				if (host.GetPeerStatus(guids[i]) != ReplicationPeerStatus::Active)
					return false;
			return true; }, 10000, 5);
		if (!active)
		{
			state.SkipWithError("clients did not become active");
			return;
		}
	}
	InterestGrid &grid = host.GetInterestGrid();
	grid.Configure({{0, 0}, {kWorld, kWorld}, 50});
	for (unsigned i = 0; i < kClients; ++i)
		grid.SetObserverRadius(guids[i], Spread(i, kClients, 0x5eed), 200, 250);
	std::vector<std::unique_ptr<Npc>> npcs;
	DelegationPolicy policy;
	for (unsigned i = 0; i < kNpcs; ++i)
	{
		npcs.push_back(std::make_unique<Npc>());
		if (!host.Reference(npcs.back().get(), SpatialScope{Spread(i, kNpcs, 0xbeef)}) || !host.SetDelegation(*npcs.back(), &policy))
		{
			state.SkipWithError("delegation setup failed");
			return;
		}
	}
	// Every replica elected and every construction delivered before measuring.
	host.SetDelegationInterval(100);
	net.Step(3000);
	host.SetTickMode(ReplicationTickMode::Manual);
	const uint32_t intervals[] = {UINT32_MAX, 500, 0};
	host.SetDelegationInterval(intervals[state.range(0)]);
	const DelegationStatistics before = host.GetDelegationStatistics();
	for (auto _ : state)
	{
		host.Tick();
		state.PauseTiming();
		net.Step(kTickMs);
		state.ResumeTiming();
	}
	const DelegationStatistics after = host.GetDelegationStatistics();
	const double ticks = static_cast<double>(state.iterations());
	state.counters["elections/tick"] = static_cast<double>(after.elections - before.elections) / ticks;
	state.counters["handovers/tick"] = static_cast<double>(after.handovers - before.handovers) / ticks;
	state.counters["simulated"] = static_cast<double>(after.simulated);
	for (auto &npc : npcs)
		host.Dereference(npc.get());
}
BENCHMARK(BM_DelegationPass)->ArgName("pass")->DenseRange(0, 2)->Unit(benchmark::kMicrosecond)->Iterations(300);
} // namespace
