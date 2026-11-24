// Copyright (c) 2026, MafiaHub. MIT license.
#include <benchmark/benchmark.h>
#include "SimulatedNetwork.h"
#include "../Source/src/replication_wire.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/snapshot_replica.h"
#include <algorithm>
#include <array>
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
		return stats.lifecycleBytes + stats.stateBytes + stats.clockBytes;
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
// Sample the age of the state actually installed on each client, in server ticks.
void BM_StreamingObserverCadence(benchmark::State &state)
{
	constexpr unsigned count = 32;
	Scene scene(count);
	if (!scene.Start(1))
	{
		state.SkipWithError("session setup failed");
		return;
	}
	for (auto *client : scene.clients)
	{
		const auto observer = ToPeerGuid(client->GetMyGUID());
		scene.host.GetInterestGrid().SetObserverCells(observer, {{0, 0}});
		if (!scene.host.SetObserverStateBudget(observer, static_cast<uint32_t>(state.range(0))))
		{
			state.SkipWithError("budget setup failed");
			return;
		}
		for (unsigned i = 8; i < count; ++i)
			scene.host.SetUpdateInterval(observer, scene.actors[i], 6);
	}
	scene.host.Tick();
	if (!scene.Converged(count, [](unsigned, unsigned)
	                     { return true; }))
	{
		state.SkipWithError("baseline mismatch");
		return;
	}
	unsigned tick = 0;
	std::vector<double> elapsed;
	std::array<std::vector<unsigned>, 2> ages;
	const auto before = scene.Bytes();
	for (auto _ : state)
	{
		++tick;
		for (auto &actor : scene.actors)
			actor.state.x = static_cast<float>(tick);
		elapsed.push_back(scene.Tick());
		state.SetIterationTime(elapsed.back());
		scene.net.Step(16);
		for (auto *client : scene.clients)
			for (unsigned i = 0; i < count; ++i)
			{
				auto *copy = static_cast<Actor *>(client->GetReplication().FindReplica(scene.actors[i].GetNetworkID()));
				if (!copy)
				{
					state.SkipWithError("visible actor disappeared");
					return;
				}
				ages[i >= 8].push_back(tick - static_cast<unsigned>(copy->state.x));
			}
	}
	Counters(state, elapsed, scene.Bytes() - before);
	for (unsigned i = 0; i < ages.size(); ++i)
	{
		auto &values = ages[i];
		std::sort(values.begin(), values.end());
		state.counters[i ? "far_p95_age_ticks" : "near_p95_age_ticks"] = values[values.size() - values.size() / 20 - 1];
	}
}
class FeedbackPeer final : public RakPeer
{
public:
	using RakPeer::Send;
	using RakPeer::SendDatagram;
	uint64_t snapshotAcks = 0, repairs = 0, replicationBytes = 0;
	void CountSnapshotMessage(const BitStream &data, Reliability reliability)
	{
		BitStream in(data.GetData(), data.GetNumberOfBytesUsed(), false);
		ReplicationWire::Header header;
		if (!ReplicationWire::ReadHeader(in, header))
			return;
		replicationBytes += data.GetNumberOfBytesUsed();
		const auto kind = header.kind;
		snapshotAcks += ReplicationWire::IsSnapshotAck(kind);
		repairs += kind == ReplicationWire::Kind::Snapshot && reliability == Reliability::ReliableOrdered;
	}
	DatagramSendResult SendDatagram(const BitStream &data, Priority priority, Reliability reliability, char channel, const AddressOrGUID target) override
	{
		auto result = RakPeer::SendDatagram(data, priority, reliability, channel, target);
		if (result.status == DatagramSendStatus::Queued)
			CountSnapshotMessage(data, reliability);
		return result;
	}
	uint32_t Send(const BitStream *data, Priority priority, Reliability reliability, char channel, const AddressOrGUID target, bool broadcast, uint32_t receipt) override
	{
		const auto result = RakPeer::Send(data, priority, reliability, channel, target, broadcast, receipt);
		if (result)
			CountSnapshotMessage(*data, reliability);
		return result;
	}
};

template <class Replica>
bool StartFeedbackSession(benchmark::State &state, SimulatedNetwork &net, FeedbackPeer *server, FeedbackPeer *client, std::vector<Replica> &actors)
{
	auto &host = server->GetReplication(), &view = client->GetReplication();
	for (auto *peer : {server, client})
	{
		auto &side = peer->GetReplication();
		if (!side.SetRole(peer == server ? ReplicationRole::Server : ReplicationRole::Client) ||
		    !side.template RegisterReplica<Replica>(actorType, 1) ||
		    !side.SetSnapshotPolicy(actorType, ChangeDetection::Explicit, SnapshotDelivery::Replaceable))
		{
			state.SkipWithError("snapshot configuration failed");
			return false;
		}
	}
	for (auto &actor : actors)
		if (!host.Reference(&actor))
		{
			state.SkipWithError("reference failed");
			return false;
		}
	if (!net.Connect(client, server) || !net.RunUntil([&]
	                                                  { return view.GetReplicaCount() == actors.size(); }, 5000))
	{
		state.SkipWithError("snapshot baseline missing");
		return false;
	}
	net.Step(500);
	host.SetTickMode(ReplicationTickMode::Manual);
	host.SetSerializeInterval(0);
	return true;
}

// One update per simulated second leaves time for reliable repair.
// Count UDP payload bytes at the socket, including control traffic in both directions.
void BM_SnapshotFeedback(benchmark::State &state)
{
	const auto count = static_cast<unsigned>(state.range(0));
	std::vector<Actor> actors(count);
	uint64_t forwardBytes = 0, reverseBytes = 0, reversePackets = 0, convergence = 0;
	SimulatedNetwork net(static_cast<uint32_t>(state.range(2)));
	auto *server = static_cast<FeedbackPeer *>(net.AddCustomPeer(std::make_unique<FeedbackPeer>(), 1, 1));
	auto *client = static_cast<FeedbackPeer *>(net.AddCustomPeer(std::make_unique<FeedbackPeer>(), 1));
	auto &host = server->GetReplication(), &view = client->GetReplication();
	if (!StartFeedbackSession(state, net, server, client, actors))
		return;
	SimulatedNetwork::LinkRule rule;
	rule.lossPercent = static_cast<int>(state.range(1));
	net.SetLinkBothWays(client, server, rule);
	const auto reverse = net.AddressOf(client);
	net.SetTap([&, reverse](const SystemAddress &from, const SystemAddress &, const char *, int length)
	           {
		if (from == reverse)
		{
			reverseBytes += length;
			++reversePackets;
		}
		else
			forwardBytes += length; });
	client->snapshotAcks = server->repairs = 0;
	server->replicationBytes = client->replicationBytes = 0;
	for (auto _ : state)
	{
		for (auto &actor : actors)
		{
			++actor.state.x;
			if (!host.MarkDirty(actor))
			{
				state.SkipWithError("failed to schedule snapshot");
				return;
			}
		}
		bool delivered = false;
		for (unsigned ms = 10; ms <= 1000; ms += 10)
		{
			host.Tick();
			net.Step(10);
			if (!delivered && std::all_of(actors.begin(), actors.end(), [&](Actor &actor)
			                              {auto *copy = static_cast<Actor *>(view.FindReplica(actor.GetNetworkID())); return copy && copy->state.x == actor.state.x; }))
			{
				delivered = true;
				convergence += ms;
			}
		}
		if (!delivered)
		{
			state.SkipWithError("snapshot failed to converge during observation window");
			return;
		}
	}
	const double n = static_cast<double>(state.iterations());
	state.counters["forward_replication_bytes"] = server->replicationBytes / n;
	state.counters["reverse_replication_bytes"] = client->replicationBytes / n;
	state.counters["reverse_ack_messages"] = client->snapshotAcks / n;
	state.counters["reverse_udp_datagrams"] = reversePackets / n;
	state.counters["reverse_udp_bytes"] = reverseBytes / n;
	state.counters["total_udp_bytes"] = (forwardBytes + reverseBytes) / n;
	state.counters["repair_messages"] = server->repairs / n;
	state.counters["convergence_ms"] = convergence / n;
}
void SnapshotFeedbackCases(benchmark::Benchmark *bench)
{
	for (int objects : {1, 32})
	{
		bench->Args({objects, 0, 19});
		for (int seed = 19; seed <= 26; ++seed)
			bench->Args({objects, 5, seed});
	}
}
BENCHMARK(BM_SnapshotFeedback)->Apply(SnapshotFeedbackCases)->Iterations(64);

// Same three floats as Actor, with the last two on a separate priority.
struct SplitActor : Actor
{
	RM3SerializationResult Serialize(SerializeParameters *params) override
	{
		params->outputBitstream[0].Write(state.x);
		params->pro[2].priority = Priority::Medium;
		params->outputBitstream[2].Write(state.y);
		params->outputBitstream[2].Write(state.z);
		return RM3SR_BROADCAST_IDENTICALLY;
	}
	void Deserialize(DeserializeParameters *params) override
	{
		if (params->bitstreamWrittenTo[0])
			params->bitstreamWrittenTo[0] = params->serializationBitstream[0].Read(state.x);
		if (params->bitstreamWrittenTo[2])
		{
			auto &in = params->serializationBitstream[2];
			float y, z;
			if (in.Read(y) && in.Read(z))
			{
				state.y = y;
				state.z = z;
			}
		}
	}
};

// Sample the age of the oldest applied coordinate every virtual millisecond.
// Forward blackouts cover the last 100 ms of each second, including the final update.
// Keep ticking after publishing stops so the sender can repair that final loss.
template <class Replica>
void BM_SnapshotFreshness(benchmark::State &state)
{
	const auto hz = static_cast<unsigned>(state.range(0));
	const auto blackoutMs = static_cast<unsigned>(state.range(1));
	constexpr unsigned duration = 8000, deadline = 2000;
	for (auto _ : state)
	{
		std::vector<Replica> actors(1);
		uint64_t udpBytes = 0;
		SimulatedNetwork net(19);
		auto *server = static_cast<FeedbackPeer *>(net.AddCustomPeer(std::make_unique<FeedbackPeer>(), 1, 1));
		auto *client = static_cast<FeedbackPeer *>(net.AddCustomPeer(std::make_unique<FeedbackPeer>(), 1));
		if (!StartFeedbackSession(state, net, server, client, actors))
			return;
		SimulatedNetwork::LinkRule link;
		link.delayMs = static_cast<unsigned>(state.range(2));
		net.SetLink(server, client, link);
		auto reverse = link;
		reverse.lossPercent = static_cast<int>(state.range(3));
		reverse.jitterMs = static_cast<unsigned>(state.range(4));
		net.SetLink(client, server, reverse);
		net.SetTap([&](const SystemAddress &, const SystemAddress &, const char *, int bytes)
		           { udpBytes += bytes; });
		auto &host = server->GetReplication();
		auto &actor = actors.front();
		const auto *copy = static_cast<const Replica *>(client->GetReplication().FindReplica(actor.GetNetworkID()));
		if (!copy)
		{
			state.SkipWithError("missing client replica");
			return;
		}
		std::vector<unsigned> ages, blackoutAges;
		ages.reserve(duration);
		unsigned tick = 1;
		unsigned finalMs = deadline;
		bool converged = false;
		server->repairs = 0;
		for (unsigned ms = 1; ms <= duration + deadline; ++ms)
		{
			const bool dark = ms <= duration && (ms - 1) % 1000 >= 1000 - blackoutMs;
			// Let the final packet leave the send queue while the link is still dark.
			const bool finalBlackout = blackoutMs && ms > duration && ms <= duration + 25;
			if (link.dark != (dark || finalBlackout))
			{
				link.dark = dark || finalBlackout;
				net.SetLink(server, client, link);
			}
			net.Step(1);
			if (ms == tick * 1000 / hz)
			{
				if (ms <= duration)
				{
					const auto stamp = static_cast<float>(ms);
					actor.state = {stamp, stamp, stamp};
					if (!host.MarkDirty(actor))
					{
						state.SkipWithError("failed to mark snapshot dirty");
						return;
					}
				}
				host.Tick();
				++tick;
			}
			const auto &value = copy->state;
			const auto oldest = static_cast<unsigned>(std::min({value.x, value.y, value.z}));
			if (ms <= duration)
			{
				ages.push_back(ms - oldest);
				if (dark)
					blackoutAges.push_back(ms - oldest);
			}
			if (!converged && oldest == duration)
			{
				finalMs = ms - duration;
				converged = true;
			}
		}
		auto p95 = [](std::vector<unsigned> &values)
		{
			std::sort(values.begin(), values.end());
			return values[values.size() - values.size() / 20 - 1];
		};
		state.counters["p95_age_ms"] = p95(ages);
		state.counters["p95_blackout_age_ms"] = blackoutAges.empty() ? 0 : p95(blackoutAges);
		state.counters["final_converged"] = converged;
		state.counters["final_convergence_ms"] = finalMs;
		state.counters["repair_messages"] = static_cast<double>(server->repairs);
		state.counters["total_udp_bytes"] = static_cast<double>(udpBytes);
	}
}
void SnapshotFreshnessCases(benchmark::Benchmark *bench)
{
	bench->ArgNames({"Hz", "blackout_ms", "one_way_ms", "reverse_loss_pct", "reverse_jitter_ms"});
	for (int hz : {30, 60})
	{
		bench->Args({hz, 0, 20, 0, 0});
		bench->Args({hz, 100, 20, 0, 0});
		bench->Args({hz, 0, 150, 0, 0});
		bench->Args({hz, 100, 20, 10, 0});
		bench->Args({hz, 100, 20, 0, 40});
		bench->Args({hz, 100, 20, 10, 40});
	}
}
BENCHMARK_TEMPLATE(BM_SnapshotFreshness, Actor)->Apply(SnapshotFreshnessCases)->Iterations(1);
BENCHMARK_TEMPLATE(BM_SnapshotFreshness, SplitActor)->Apply(SnapshotFreshnessCases)->Iterations(1);
BENCHMARK(BM_StreamingObserverCadence)->Arg(0)->Arg(768)->UseManualTime()->Iterations(100)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StreamingDenseMutations)->UseManualTime()->Iterations(100)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StreamingRoomCrossing)->UseManualTime()->Iterations(50)->Unit(benchmark::kMicrosecond);
}
