// Copyright (c) 2026, MafiaHub. MIT license.
#include <benchmark/benchmark.h>
#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/snapshot_replica.h"
#include <algorithm>
#include <chrono>
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
constexpr unsigned observers = 20;
constexpr ReplicaTypeId actorType{1};
using Clock = std::chrono::steady_clock;

class MeteredPeer final : public RakPeer
{
public:
	using RakPeer::Send;
	uint64_t enqueues = 0;
	uint32_t Send(const BitStream *data, Priority priority, Reliability reliability, char channel, const AddressOrGUID target, bool broadcast, uint32_t receipt) override
	{
		const auto result = RakPeer::Send(data, priority, reliability, channel, target, broadcast, receipt);
		if (result && data->GetNumberOfBytesUsed() && data->GetData()[0] == ID_REPLICATION)
			++enqueues;
		return result;
	}
};

class Scene
{
public:
	explicit Scene(unsigned count)
	    : actors(count), server(static_cast<MeteredPeer *>(net.AddCustomPeer(std::make_unique<MeteredPeer>(), observers, observers))), host(server->GetReplication()) {}
	bool Start(unsigned cells)
	{
		if (!host.SetRole(ReplicationRole::Server) || !host.RegisterReplica<Actor>(actorType, 1) ||
		    !host.GetInterestGrid().Configure({{0, 0}, {static_cast<float>(cells), 1}, 1}))
			return false;
		host.SetTickMode(ReplicationTickMode::Manual);
		host.SetSerializeInterval(0);
		for (unsigned i = 0; i < actors.size(); ++i)
			if (!host.Reference(&actors[i], SpatialScope{{static_cast<float>(i % cells) + 0.5f, 0.5f}}))
				return false;
		for (unsigned i = 0; i < observers; ++i)
		{
			auto *peer = net.AddPeer(1);
			auto &remote = peer->GetReplication();
			if (!remote.SetRole(ReplicationRole::Client) || !remote.RegisterReplica<Actor>(actorType, 1) || !net.Connect(peer, server))
				return false;
			clients.push_back(peer);
		}
		return net.RunUntil([&]
		                    { return host.GetConnectionCount() == observers; }, 5000);
	}
	double Tick()
	{
		const auto start = Clock::now();
		host.Tick();
		return std::chrono::duration<double>(Clock::now() - start).count();
	}
	uint64_t Bytes() const
	{
		const auto stats = host.GetStatistics();
		return stats.lifecycleBytes + stats.stateBytes;
	}
	// Verify identity, count and decoded values for every client outside the timed region.
	template <typename Visible>
	bool Converged(unsigned count, Visible visible)
	{
		auto matches = [&]
		{
			for (unsigned i = 0; i < clients.size(); ++i)
			{
				auto &remote = clients[i]->GetReplication();
				if (remote.GetReplicaCount() != count)
					return false;
				for (unsigned j = 0; j < actors.size(); ++j)
				{
					if (!visible(i, j))
						continue;
					auto *copy = static_cast<Actor *>(remote.FindReplica(actors[j].GetNetworkID()));
					if (!copy || copy->state.x != actors[j].state.x)
						return false;
				}
			}
			return true;
		};
		return net.RunUntil(matches, 30000, 16);
	}
	std::vector<Actor> actors;
	SimulatedNetwork net;
	MeteredPeer *server;
	ReplicationManager &host;
	std::vector<RakPeer *> clients;
};

void Counters(benchmark::State &state, std::vector<double> &samples, uint64_t bytes)
{
	if (samples.empty())
		return;
	std::sort(samples.begin(), samples.end());
	const auto rank = samples.size() - (samples.size() / 20) - 1;
	state.counters["p95_tick_us"] = samples[rank] * 1e6;
	state.counters["bytes_per_tick"] = static_cast<double>(bytes) / samples.size();
}

void BM_StreamingDenseMutations(benchmark::State &state)
{
	constexpr unsigned count = 500, visible = 390;
	Scene scene(count);
	if (!scene.Start(count))
	{
		state.SkipWithError("session setup failed");
		return;
	}
	auto selected = [](unsigned client, unsigned actor)
	{ return (actor + count - client) % count >= count - visible; };
	for (unsigned i = 0; i < observers; ++i)
	{
		std::vector<GridCell> cells;
		for (unsigned j = 0; j < count; ++j)
			if (selected(i, j))
				cells.push_back({static_cast<int>(j), 0});
		if (!scene.host.GetInterestGrid().SetObserverCells(ToPeerGuid(scene.clients[i]->GetMyGUID()), cells))
		{
			state.SkipWithError("dense visibility setup failed");
			return;
		}
	}
	scene.host.Tick();
	if (!scene.Converged(visible, selected))
	{
		state.SkipWithError("baseline mismatch");
		return;
	}
	const auto before = scene.Bytes();
	const auto enqueuesBefore = scene.server->enqueues;
	std::vector<double> samples;
	for (auto _ : state)
	{
		for (auto &actor : scene.actors)
			actor.state.x += 1;
		samples.push_back(scene.Tick());
		state.SetIterationTime(samples.back());
		if (!scene.Converged(visible, selected))
		{
			state.SkipWithError("state mismatch");
			break;
		}
	}
	Counters(state, samples, scene.Bytes() - before);
	if (!samples.empty())
		state.counters["enqueues_per_tick"] = static_cast<double>(scene.server->enqueues - enqueuesBefore) / samples.size();
}

// Twenty players cross a room boundary together. Each room contains 100 actors,
// with a construction budget of ten per player per 16 ms simulation frame.
void BM_StreamingRoomCrossing(benchmark::State &state)
{
	Scene scene(200);
	if (!scene.Start(2))
	{
		state.SkipWithError("session setup failed");
		return;
	}
	for (auto *client : scene.clients)
		scene.host.GetInterestGrid().SetObserverLimits(ToPeerGuid(client->GetMyGUID()), 0, 10);
	unsigned room = 0;
	std::vector<double> samples;
	unsigned worstTicks = 0;
	const auto before = scene.Bytes();
	for (auto _ : state)
	{
		for (auto *client : scene.clients)
			scene.host.GetInterestGrid().SetObserverCells(ToPeerGuid(client->GetMyGUID()), {{static_cast<int>(room), 0}});
		double elapsed = 0;
		unsigned ticks = 0;
		bool ready = false;
		while (!ready && ticks < 120)
		{
			const double tick = scene.Tick();
			samples.push_back(tick);
			elapsed += tick;
			++ticks;
			scene.net.Step(16);
			ready = std::all_of(scene.clients.begin(), scene.clients.end(), [&](RakPeer *client)
			                    {
				auto &remote=client->GetReplication();
				if(remote.GetReplicaCount()!=100) return false;
				for(unsigned i=room;i<scene.actors.size();i+=2)
					if (!remote.FindReplica(scene.actors[i].GetNetworkID())) return false;
				return true; });
		}
		state.SetIterationTime(elapsed);
		if (!ready)
		{
			state.SkipWithError("room did not converge");
			break;
		}
		worstTicks = std::max(worstTicks, ticks);
		room ^= 1;
	}
	Counters(state, samples, scene.Bytes() - before);
	state.counters["ticks_to_last_baseline"] = worstTicks;
}
BENCHMARK(BM_StreamingDenseMutations)->UseManualTime()->Iterations(100)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StreamingRoomCrossing)->UseManualTime()->Iterations(50)->Unit(benchmark::kMicrosecond);
}
