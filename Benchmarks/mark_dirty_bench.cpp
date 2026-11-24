// Copyright (c) 2026, MafiaHub. MIT license.
#include <benchmark/benchmark.h>
#include "SimulatedNetwork.h"
#include "mafianet/snapshot_replica.h"
#include <chrono>
#include <memory>
#include <vector>

using namespace MafiaNet;

namespace
{
struct Position
{
	float x = 0, y = 0, z = 0;
	void Write(BitStream &out) const
	{
		out.Write(x);
		out.Write(y);
		out.Write(z);
	}
	bool Read(BitStream &in) { return in.Read(x) && in.Read(y) && in.Read(z); }
};
struct Actor : SnapshotReplica<Position>
{
};
constexpr ReplicaTypeId actorType{1};
constexpr unsigned actorCount = 1024, observersPerActor = 4;
using Clock = std::chrono::steady_clock;

bool Explicit(ReplicationManager &replication, ReplicationRole role)
{
	return replication.SetRole(role) && replication.RegisterReplica<Actor>(actorType, 1) &&
	       replication.SetSnapshotPolicy(actorType, ChangeDetection::Explicit, SnapshotDelivery::Reliable);
}
}

// MarkDirty() on a server with many connections, where interest management
// shows each actor to only four of them: the cost of a mark should follow the
// observers that have the actor, not the number of connections.
void BM_MarkDirtyFewObservers(benchmark::State &state)
{
	const auto connections = static_cast<unsigned>(state.range(0));
	const unsigned cells = connections / observersPerActor;
	SimulatedNetwork net(7);
	auto *server = net.AddPeer(connections, static_cast<unsigned short>(connections));
	auto &host = server->GetReplication();
	if (!Explicit(host, ReplicationRole::Server) || !host.GetInterestGrid().Configure({{0, 0}, {static_cast<float>(cells), 1}, 1}))
	{
		state.SkipWithError("server setup failed");
		return;
	}
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	std::vector<std::unique_ptr<Actor>> actors;
	for (unsigned i = 0; i < actorCount; ++i)
	{
		actors.push_back(std::make_unique<Actor>());
		if (!host.Reference(actors.back().get(), SpatialScope{{static_cast<float>(i % cells) + 0.5f, 0.5f}}))
		{
			state.SkipWithError("reference failed");
			return;
		}
	}
	std::vector<RakPeer *> clients;
	for (unsigned i = 0; i < connections; ++i)
	{
		auto *peer = net.AddPeer(1);
		if (!Explicit(peer->GetReplication(), ReplicationRole::Client) || net.StartConnect(peer, server) != CONNECTION_ATTEMPT_STARTED)
		{
			state.SkipWithError("client setup failed");
			return;
		}
		clients.push_back(peer);
	}
	if (!net.RunUntil([&]
	                  { return host.GetConnectionCount() == connections; }, 60000))
	{
		state.SkipWithError("sessions did not start");
		return;
	}
	for (unsigned i = 0; i < connections; ++i)
		host.GetInterestGrid().SetObserverCells(ToPeerGuid(clients[i]->GetMyGUID()), {{static_cast<int>(i % cells), 0}});
	const uint64_t constructions = uint64_t{actorCount} * observersPerActor;
	if (!net.RunUntil([&]
	                  {
		                  host.Tick();
		                  return host.GetStatistics().constructed == constructions; }, 60000, 16))
	{
		state.SkipWithError("constructions did not complete");
		return;
	}
	double marked = 0;
	for (auto _ : state)
	{
		for (auto &actor : actors)
			++actor->state.x;
		const auto start = Clock::now();
		for (auto &actor : actors)
			if (!host.MarkDirty(*actor))
			{
				state.SkipWithError("mark failed");
				return;
			}
		const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
		state.SetIterationTime(elapsed);
		marked += elapsed;
		// Drain the marks outside the timed region so every iteration starts clean.
		host.Tick();
		net.Step(16);
	}
	state.counters["ns_per_mark"] = marked * 1e9 / (static_cast<double>(state.iterations()) * actorCount);
}
BENCHMARK(BM_MarkDirtyFewObservers)->ArgName("connections")->Arg(8)->Arg(64)->Arg(256)->Arg(800)->UseManualTime()->Iterations(50)->Unit(benchmark::kMicrosecond);
