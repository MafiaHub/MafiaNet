// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/interest_grid.h"
#include "mafianet/replication_manager.h"
#include "mafianet/snapshot_replica.h"
#include "SimulatedNetwork.h"
#include <benchmark/benchmark.h>
#include <memory>
#include <vector>

using namespace MafiaNet;

namespace
{
void InterestGridWorkload(benchmark::State &state)
{
	InterestGrid grid;
	grid.Configure({{0, 0}, {1024, 1024}, 16});
	for (NetworkID i = 0; i < 50000; ++i)
		grid.Track(i, {{static_cast<float>(i % 250) * 4, static_cast<float>(i / 250) * 5}});
	std::vector<NetworkID> visible;
	for (unsigned i = 0; i < 20; ++i)
	{
		auto guid = PeerGuid{i + 1};
		grid.SetObserverRadius(guid, {150.0f + i * 15, 150}, 36, 42);
		grid.SetObserverLimits(guid, 100, 16);
		grid.GetVisible(guid, visible);
	}
	const auto before = grid.GetStatistics();
	bool alternate = false;
	for (auto _ : state)
	{
		alternate = !alternate;
		switch (state.range(0))
		{
		case 1:
			grid.SetPosition(49999, {alternate ? 991.f : 992.f, 991});
			break;
		case 2:
			grid.SetPosition(8000, {alternate ? 151.f : 152.f, 150});
			break;
		case 3:
			for (unsigned i = 0; i < 20; ++i)
				grid.SetObserverPosition(PeerGuid{i + 1}, {alternate ? 300.f : 700.f, 150});
			break;
		}
		for (unsigned i = 0; i < 20; ++i)
		{
			grid.GetVisible(PeerGuid{i + 1}, visible);
			benchmark::DoNotOptimize(visible);
		}
	}
	const auto after = grid.GetStatistics();
	state.counters["candidates/tick"] = double(after.candidatesInspected - before.candidatesInspected) / state.iterations();
	state.counters["rebuilds/tick"] = double(after.observersRebuilt - before.observersRebuilt) / state.iterations();
}
BENCHMARK(InterestGridWorkload)->ArgName("motion")->DenseRange(0, 3);

// Categories, entity ranges, look-ahead and dependencies all enabled.
void InterestGridParityFeatures(benchmark::State &state)
{
	InterestGrid grid;
	grid.Configure({{0, 0}, {1024, 1024}, 16});
	for (NetworkID i = 0; i < 50000; ++i)
	{
		SpatialScope scope{{static_cast<float>(i % 250) * 4, static_cast<float>(i / 250) * 5}};
		scope.category = static_cast<uint16_t>(1 + i % 4);
		scope.range = i % 10 == 0 ? 60.0f : 0.0f;
		grid.Track(i + 1, scope);
	}
	grid.SetCategoryLimit(1, 40);
	grid.SetCategoryLimit(2, 40);
	grid.SetCategoryUsesEntityRange(3, true);
	grid.SetLookaheadSeconds(1);
	// Children far away follow parents inside the observers' area.
	for (NetworkID i = 0; i < 250; ++i)
	{
		grid.SetDependency(40001 + i, 7501 + i);
	}
	grid.SetAlwaysVisible(100, true);
	std::vector<NetworkID> visible;
	for (unsigned i = 0; i < 20; ++i)
	{
		auto guid = PeerGuid{i + 1};
		grid.SetObserverRadius(guid, {150.0f + i * 15, 150}, 36, 42);
		grid.SetObserverVelocity(guid, {20, 10});
		grid.SetObserverLimits(guid, 100, 16);
		grid.GetVisible(guid, visible);
	}
	const auto before = grid.GetStatistics();
	bool alternate = false;
	for (auto _ : state)
	{
		alternate = !alternate;
		for (unsigned i = 0; i < 20; ++i)
		{
			grid.SetObserverPosition(PeerGuid{i + 1}, {alternate ? 300.f : 700.f, 150});
		}
		for (unsigned i = 0; i < 20; ++i)
		{
			grid.GetVisible(PeerGuid{i + 1}, visible);
			benchmark::DoNotOptimize(visible);
		}
	}
	const auto after = grid.GetStatistics();
	state.counters["candidates/tick"] = double(after.candidatesInspected - before.candidatesInspected) / state.iterations();
	state.counters["rebuilds/tick"] = double(after.observersRebuilt - before.observersRebuilt) / state.iterations();
	state.counters["depPulls/tick"] = double(after.dependencyPulls - before.dependencyPulls) / state.iterations();
}
BENCHMARK(InterestGridParityFeatures);

// A crowd moving every tick: 800 radius observers on 800 avatars and 200 vehicles in a 4,000 x 4,000 world, 25% of
// the avatars being drivers, dependency children of their vehicle. Every replica moves, then every observer is
// evaluated, as a 30 Hz server tick does. The arg sets whether observers follow their avatar (1, every observer is
// rebuilt every tick) or stay put (0, only the observers a move can affect are rebuilt).
constexpr unsigned kCrowdObservers = 800;
constexpr unsigned kCrowdVehicles = 200;
constexpr float kCrowdWorld = 4000;

struct Crowd
{
	std::vector<SpatialPoint> positions, steps;
	// Spreads index i of count over the world on a jittered lattice, deterministic.
	static SpatialPoint Spread(unsigned i, unsigned count, uint32_t salt)
	{
		unsigned side = 1;
		while (side * side < count)
			++side;
		const float cell = kCrowdWorld / static_cast<float>(side);
		uint32_t hash = (i + 1) * 2654435761u ^ salt;
		hash ^= hash >> 15;
		const float jx = static_cast<float>(hash & 0xff) / 255.0f, jy = static_cast<float>((hash >> 8) & 0xff) / 255.0f;
		return {(static_cast<float>(i % side) + jx) * cell, (static_cast<float>(i / side) + jy) * cell};
	}
	// Replica ids: avatars 1..800, vehicles 801..1000; avatar i (i <= 200) drives vehicle 800 + i.
	Crowd()
	{
		for (unsigned i = 0; i < kCrowdObservers + kCrowdVehicles; ++i)
		{
			const bool vehicle = i >= kCrowdObservers;
			const unsigned driver = i - kCrowdObservers;
			positions.push_back(vehicle ? positions[driver] : Spread(i, kCrowdObservers, 0x5eed));
			const uint32_t hash = (i + 7) * 2246822519u;
			const float speed = vehicle ? 1.0f : 0.15f;
			steps.push_back({(static_cast<float>(hash & 0xff) / 127.5f - 1) * speed, (static_cast<float>((hash >> 8) & 0xff) / 127.5f - 1) * speed});
		}
		// Drivers ride with their vehicle.
		for (unsigned i = 0; i < kCrowdVehicles; ++i)
			steps[i] = steps[kCrowdObservers + i];
	}
	static NetworkID Id(unsigned i) { return i + 1; }
	static PeerGuid Observer(unsigned i) { return PeerGuid{i + 1}; }
	void Step()
	{
		for (size_t i = 0; i < positions.size(); ++i)
		{
			auto &p = positions[i];
			auto &s = steps[i];
			if (p.x + s.x < 0 || p.x + s.x > kCrowdWorld)
				s.x = -s.x;
			if (p.y + s.y < 0 || p.y + s.y > kCrowdWorld)
				s.y = -s.y;
			p.x += s.x;
			p.y += s.y;
		}
	}
};

void InterestGridCrowdWithDrivers(benchmark::State &state)
{
	InterestGrid grid;
	grid.Configure({{0, 0}, {kCrowdWorld, kCrowdWorld}, 50});
	Crowd crowd;
	for (unsigned i = 0; i < crowd.positions.size(); ++i)
		grid.Track(Crowd::Id(i), {crowd.positions[i]});
	for (unsigned i = 0; i < kCrowdVehicles; ++i)
		grid.SetDependency(Crowd::Id(i), Crowd::Id(kCrowdObservers + i));
	std::vector<NetworkID> visible;
	for (unsigned i = 0; i < kCrowdObservers; ++i)
	{
		grid.SetObserverRadius(Crowd::Observer(i), crowd.positions[i], 200, 250);
		grid.GetVisible(Crowd::Observer(i), visible);
	}
	const auto before = grid.GetStatistics();
	for (auto _ : state)
	{
		crowd.Step();
		for (unsigned i = 0; i < crowd.positions.size(); ++i)
			grid.SetPosition(Crowd::Id(i), crowd.positions[i]);
		if (state.range(0))
		{
			for (unsigned i = 0; i < kCrowdObservers; ++i)
				grid.SetObserverPosition(Crowd::Observer(i), crowd.positions[i]);
		}
		for (unsigned i = 0; i < kCrowdObservers; ++i)
		{
			grid.GetVisible(Crowd::Observer(i), visible);
			benchmark::DoNotOptimize(visible);
		}
	}
	const auto after = grid.GetStatistics();
	state.counters["rebuilds/tick"] = double(after.observersRebuilt - before.observersRebuilt) / state.iterations();
	state.counters["depPulls/tick"] = double(after.dependencyPulls - before.dependencyPulls) / state.iterations();
}
BENCHMARK(InterestGridCrowdWithDrivers)->ArgName("observers")->DenseRange(0, 1)->Unit(benchmark::kMicrosecond);

// Observers report their position and velocity every tick, as a server does for each player, moving a little (0.15
// units a tick in 50-unit cells, so most stay in their cell and keep their cell set). Objects stand still at the
// observers' start positions. The first arg is the observer count; the second picks whether the tick then evaluates
// every observer (1) or only applies the updates (0). Look-ahead is off, so the velocity cannot change any result.
void InterestGridObserverUpdates(benchmark::State &state)
{
	const auto count = static_cast<unsigned>(state.range(0));
	InterestGrid grid;
	grid.Configure({{0, 0}, {kCrowdWorld, kCrowdWorld}, 50});
	std::vector<SpatialPoint> positions, steps;
	for (unsigned i = 0; i < count; ++i)
	{
		positions.push_back(Crowd::Spread(i, count, 0x0b5e));
		const uint32_t hash = (i + 7) * 2246822519u;
		steps.push_back({(static_cast<float>(hash & 0xff) / 127.5f - 1) * 0.15f, (static_cast<float>((hash >> 8) & 0xff) / 127.5f - 1) * 0.15f});
		grid.Track(Crowd::Id(i), {positions[i]});
	}
	std::vector<NetworkID> visible;
	for (unsigned i = 0; i < count; ++i)
	{
		grid.SetObserverRadius(Crowd::Observer(i), positions[i], 200, 250);
		grid.GetVisible(Crowd::Observer(i), visible);
	}
	const auto before = grid.GetStatistics();
	for (auto _ : state)
	{
		for (unsigned i = 0; i < count; ++i)
		{
			auto &p = positions[i];
			auto &s = steps[i];
			if (p.x + s.x < 0 || p.x + s.x > kCrowdWorld)
				s.x = -s.x;
			if (p.y + s.y < 0 || p.y + s.y > kCrowdWorld)
				s.y = -s.y;
			p.x += s.x;
			p.y += s.y;
			grid.SetObserverPosition(Crowd::Observer(i), p);
			grid.SetObserverVelocity(Crowd::Observer(i), {s.x * 30, s.y * 30});
		}
		if (state.range(1))
		{
			for (unsigned i = 0; i < count; ++i)
			{
				grid.GetVisible(Crowd::Observer(i), visible);
				benchmark::DoNotOptimize(visible);
			}
		}
	}
	const auto after = grid.GetStatistics();
	state.counters["rebuilds/tick"] = double(after.observersRebuilt - before.observersRebuilt) / state.iterations();
}
BENCHMARK(InterestGridObserverUpdates)->ArgNames({"observers", "evaluate"})->ArgsProduct({{800, 1500}, {0, 1}})->Unit(benchmark::kMicrosecond);

struct CrowdState
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct CrowdReplica : SnapshotReplica<CrowdState>
{
};
constexpr ReplicaTypeId kCrowdType{78};

// The same crowd behind a server Tick on the simulated network, 800 Active clients. The arg picks how replicas enter
// the grid: 0 Reference(replica, scope); 1 Reference(replica) then GetInterestGrid().Track(), the path that keeps the
// replica on the runtime's list of ordinary replicas. Ticks are 33 ms of virtual time apart; the network steps
// between ticks, untimed.
void InterestGridCrowdServerTick(benchmark::State &state)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(kCrowdObservers, kCrowdObservers);
	auto &host = server->GetReplication();
	if (!host.SetRole(ReplicationRole::Server) || !host.RegisterReplica<CrowdReplica>(kCrowdType, 1))
	{
		state.SkipWithError("server setup failed");
		return;
	}
	std::vector<RakPeer *> clients;
	std::vector<PeerGuid> guids;
	constexpr unsigned kBatch = 16;
	for (unsigned first = 0; first < kCrowdObservers; first += kBatch)
	{
		for (unsigned i = first; i < first + kBatch && i < kCrowdObservers; ++i)
		{
			RakPeer *client = net.AddPeer(1);
			if (!client->GetReplication().SetRole(ReplicationRole::Client) || !client->GetReplication().RegisterReplica<CrowdReplica>(kCrowdType, 1) || net.StartConnect(client, server) != CONNECTION_ATTEMPT_STARTED)
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
	grid.Configure({{0, 0}, {kCrowdWorld, kCrowdWorld}, 50});
	Crowd crowd;
	for (unsigned i = 0; i < kCrowdObservers; ++i)
		grid.SetObserverRadius(guids[i], crowd.positions[i], 200, 250);
	std::vector<std::unique_ptr<CrowdReplica>> replicas;
	std::vector<NetworkID> ids;
	for (unsigned i = 0; i < crowd.positions.size(); ++i)
	{
		replicas.push_back(std::make_unique<CrowdReplica>());
		const bool referenced = state.range(0) == 0 ? host.Reference(replicas.back().get(), SpatialScope{crowd.positions[i]}) : host.Reference(replicas.back().get()) && grid.Track(replicas.back()->GetNetworkID(), {crowd.positions[i]});
		if (!referenced)
		{
			state.SkipWithError("replica setup failed");
			return;
		}
		ids.push_back(replicas.back()->GetNetworkID());
	}
	for (unsigned i = 0; i < kCrowdVehicles; ++i)
		grid.SetDependency(ids[i], ids[kCrowdObservers + i]);
	net.Step(3000);
	host.SetTickMode(ReplicationTickMode::Manual);
	for (auto _ : state)
	{
		crowd.Step();
		for (unsigned i = 0; i < crowd.positions.size(); ++i)
			grid.SetPosition(ids[i], crowd.positions[i]);
		for (unsigned i = 0; i < kCrowdObservers; ++i)
			grid.SetObserverPosition(guids[i], crowd.positions[i]);
		host.Tick();
		state.PauseTiming();
		net.Step(33);
		state.ResumeTiming();
	}
	for (auto &replica : replicas)
		host.Dereference(replica.get());
}
BENCHMARK(InterestGridCrowdServerTick)->ArgName("track")->DenseRange(0, 1)->Unit(benchmark::kMicrosecond)->Iterations(200);
}
