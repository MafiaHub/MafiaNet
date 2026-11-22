/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>

#include "mafianet/replica_manager3.h"

#include <vector>
#include "mafianet/network_id_manager.h"
#include "mafianet/message_identifiers.h"
#include <type_traits>
#include "mafianet/get_time.h"
#include "mafianet/peer.h"

using namespace MafiaNet;

namespace
{
// Count completed replication messages at the transport boundary. No socket,
// ACKs or encryption headers participate in this server tick measurement.
class CountingPeer : public RakPeer
{
public:
	using RakPeer::Send;
	uint32_t Send(const BitStream *data, Priority, Reliability, char, const AddressOrGUID, bool, uint32_t) override
	{
		++messages;
		bytes += data->GetNumberOfBytesUsed();
		return 1;
	}
	uint64_t bytes = 0;
	uint64_t messages = 0;
};

class StateReplica : public Replica3
{
public:
	uint32_t value = 0;
	bool BroadcastsIdentically() const override { return true; }
	void WriteAllocationID(Connection_RM3 *, BitStream *out) const override { out->Write(uint8_t{1}); }
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_SEND_CONSTRUCTION; }
	bool QueryRemoteConstruction(Connection_RM3 *) override { return true; }
	void SerializeConstruction(BitStream *out, Connection_RM3 *) override { out->Write(value); }
	bool DeserializeConstruction(BitStream *, Connection_RM3 *) override { return true; }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return true; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) override {}
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) override { return RM3QSR_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) override {}
	RM3SerializationResult Serialize(SerializeParameters *params) override
	{
		params->outputBitstream[0].Write(value);
		return RM3SR_BROADCAST_IDENTICALLY;
	}
};

class Connection : public Connection_RM3
{
public:
	explicit Connection(RakNetGUID guid)
	    : Connection_RM3(UNASSIGNED_SYSTEM_ADDRESS, guid) { isValidated = true; }
	Replica3 *AllocReplica(BitStream *, ReplicaManager3 *) override { return nullptr; }
};
struct SerializationCandidates
{
	std::vector<Replica3 *> changed;
	bool fullScan = false;
};

class DirtyListConnection final : public Connection
{
public:
	DirtyListConnection(RakNetGUID guid, const SerializationCandidates &candidates)
	    : Connection(guid), candidates(candidates) {}

	bool QuerySerializationList(std::vector<Replica3 *> &replicas) override
	{
		if (candidates.fullScan)
			return false;
		replicas = candidates.changed;
		return true;
	}

private:
	const SerializationCandidates &candidates;
};

class Manager : public ReplicaManager3
{
public:
	Connection_RM3 *AllocConnection(const SystemAddress &, RakNetGUID guid) const override { return new Connection(guid); }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }
};

TimeUS tickUS;
TimeUS TickClock()
{
	return tickUS;
}

template <typename Replica = StateReplica, typename Peer = CountingPeer>
struct World
{
	NetworkIDManager ids;
	Peer peer;
	Manager &manager = *peer.GetReplication().template UseLegacyProtocol<Manager>();
	std::vector<Replica> replicas;
	explicit World(int clients, size_t replicaCount = 100, const SerializationCandidates *candidates = nullptr)
	    : replicas(replicaCount)
	{
		tickUS = 1000000;
		SetTimeSourceOverride(TickClock);
		manager.SetNetworkIDManager(&ids);
		manager.SetAutoSerializeInterval(0);
		for (int i = 0; i < clients; ++i)
		{
			RakNetGUID guid(100 + i);
			if (candidates)
				manager.PushConnection(new DirtyListConnection(guid, *candidates));
			else
				manager.PushConnection(new Connection(guid));
		}
		for (auto &replica : replicas)
			manager.Reference(&replica);
	}
	~World()
	{
		SetTimeSourceOverride(nullptr);
	}
	void Tick()
	{
		tickUS += 16000;
		manager.Update();
	}
};

template <class Replica>
void MeasureConstruction(benchmark::State &state, int clients, unsigned count)
{
	uint64_t total = 0;
	for (auto _ : state)
	{
		state.PauseTiming();
		{
			World<Replica> world(clients, count);
			world.peer.bytes = 0;
			state.ResumeTiming();
			world.Tick();
			state.PauseTiming();
			if (!world.peer.bytes)
			{
				state.SkipWithError("construction emitted no messages");
				return;
			}
			total += world.peer.bytes;
		}
		state.ResumeTiming();
	}
	state.counters["rm3_bytes_per_tick"] = benchmark::Counter(total, benchmark::Counter::kAvgIterations);
}
void ReplicaManager3_Construct(benchmark::State &state)
{
	MeasureConstruction<StateReplica>(state, static_cast<int>(state.range(0)), 100);
}
BENCHMARK(ReplicaManager3_Construct)->ArgName("clients")->Arg(1)->Arg(8)->Iterations(32);

// The construction callback supplies the complete scalar value to every new
// observer. Only subsequent writes need a broadcast. A joining observer never
// consumes an update pending for the clients that already hold the replica.
class SnapshotReplica : public StateReplica
{
public:
	void SetValue(uint32_t next)
	{
		if (value != next)
		{
			value = next;
			changed = true;
		}
	}

	RM3SerializationResult Serialize(SerializeParameters *params) override
	{
		if (params->destinationConnection || !changed)
			return RM3SR_DO_NOT_SERIALIZE;
		changed = false;
		return StateReplica::Serialize(params);
	}

private:
	bool changed = false;
};

// The download markers distinguish the eager state messages sent inside
// SendConstruction from those produced by the regular pass in the same tick.
struct ConstructionTraffic : CountingPeer
{
	using CountingPeer::Send;
	uint32_t Send(const BitStream *data, Priority, Reliability, char, const AddressOrGUID, bool, uint32_t) override
	{
		const auto size = data->GetNumberOfBytesUsed();
		switch (data->GetData()[0])
		{
		case ID_REPLICA_MANAGER_DOWNLOAD_STARTED:
			downloading = true;
			controlBytes += size;
			break;
		case ID_REPLICA_MANAGER_DOWNLOAD_COMPLETE:
			downloading = false;
			controlBytes += size;
			break;
		case ID_REPLICA_MANAGER_CONSTRUCTION:
		{
			BitStream input(data->GetData(), size, false);
			input.IgnoreBytes(2); // message ID and world
			uint16_t count = 0;
			input.Read(count);
			records += count;
			constructionBytes += size;
			break;
		}
		case ID_REPLICA_MANAGER_SERIALIZE:
			if (downloading)
			{
				++initialMessages;
				initialBytes += size;
			}
			else
			{
				++regularMessages;
				regularBytes += size;
			}
			break;
		default:
			break;
		}
		bytes += size;
		return 1;
	}

	bool downloading = false;
	uint64_t controlBytes = 0, constructionBytes = 0, initialBytes = 0, regularBytes = 0;
	unsigned records = 0, initialMessages = 0, regularMessages = 0;
};

// Exercise the application dirty flag outside the sample. A mutation during a
// late join must still be sent to a client that already owns the replica.
bool SnapshotUpdatesReachExistingObservers()
{
	World<SnapshotReplica, ConstructionTraffic> world(1, 1);
	auto &traffic = world.peer;
	world.Tick();
	if (traffic.records != 1 || traffic.initialMessages != 0 || traffic.regularMessages != 0)
		return false;
	world.replicas.front().SetValue(7);
	world.manager.PushConnection(new Connection(RakNetGUID(101)));
	world.Tick();
	if (traffic.records != 2 || traffic.initialMessages != 0 || traffic.regularMessages != 2)
		return false;
	world.Tick();
	return traffic.regularMessages == 2;
}

template <class Replica>
void ConstructionBandwidth(benchmark::State &state)
{
	constexpr unsigned count = 1000;
	if constexpr (std::is_same_v<Replica, SnapshotReplica>)
	{
		if (!SnapshotUpdatesReachExistingObservers())
		{
			state.SkipWithError("snapshot lost a mutation or repeated an idle update");
			return;
		}
	}
	// Classify messages in a separate pass so the timing measures normal send
	// boundary cost, independent of the number of messages being classified.
	{
		World<Replica, ConstructionTraffic> world(1, count);
		auto &traffic = world.peer;
		traffic.bytes = 0; // exclude connection validation
		world.Tick();
		const auto stateBytes = sizeof(uint32_t) * (traffic.records + traffic.initialMessages + traffic.regularMessages);
		const auto typeBytes = sizeof(uint8_t) * traffic.records;
		if (traffic.records != count || traffic.bytes != traffic.constructionBytes + traffic.controlBytes + traffic.initialBytes + traffic.regularBytes || traffic.bytes < stateBytes + typeBytes)
		{
			state.SkipWithError("incomplete construction accounting");
			return;
		}
		state.counters["construction_records"] = traffic.records;
		state.counters["construction_bytes"] = static_cast<double>(traffic.constructionBytes);
		state.counters["initial_state_messages"] = traffic.initialMessages;
		state.counters["initial_state_bytes"] = static_cast<double>(traffic.initialBytes);
		state.counters["regular_state_messages"] = traffic.regularMessages;
		state.counters["regular_state_bytes"] = static_cast<double>(traffic.regularBytes);
		state.counters["download_control_bytes"] = static_cast<double>(traffic.controlBytes);
		state.counters["scalar_bytes"] = static_cast<double>(stateBytes);
		state.counters["allocation_id_bytes"] = static_cast<double>(typeBytes);
		state.counters["framing_bytes"] = static_cast<double>(traffic.bytes - stateBytes - typeBytes);
	}

	MeasureConstruction<Replica>(state, 1, count);
}
BENCHMARK_TEMPLATE(ConstructionBandwidth, StateReplica)->Name("ReplicaManager3_Construct1000/always_serialize")->Iterations(32);
BENCHMARK_TEMPLATE(ConstructionBandwidth, SnapshotReplica)->Name("ReplicaManager3_Construct1000/snapshot_and_dirty_updates")->Iterations(32);

void ReplicaManager3_ServerTick(benchmark::State &state)
{
	World world(static_cast<int>(state.range(0)));
	world.Tick(); // construction
	world.Tick(); // prime the per-replica serialization cache
	world.peer.bytes = 0;
	const int changed = static_cast<int>(state.range(1));
	for (auto _ : state)
	{
		for (int i = 0; i < changed; ++i)
			++world.replicas[i].value;
		world.Tick();
	}
	if ((world.peer.bytes != 0) != (changed != 0))
		state.SkipWithError("unexpected replication output");
	state.counters["rm3_bytes_per_tick"] = benchmark::Counter(world.peer.bytes, benchmark::Counter::kAvgIterations);
}
BENCHMARK(ReplicaManager3_ServerTick)->ArgsProduct({{1, 8}, {0, 1, 100}})->ArgNames({"clients", "changed_of_100"});

class PositionReplica : public StateReplica
{
public:
	float x = 1.0f, y = 2.0f, z = 3.0f;
	void SerializeConstruction(BitStream *out, Connection_RM3 *) override { WritePosition(*out); }
	RM3SerializationResult Serialize(SerializeParameters *params) override
	{
		WritePosition(params->outputBitstream[0]);
		return RM3SR_BROADCAST_IDENTICALLY;
	}

private:
	void WritePosition(BitStream &out) const
	{
		out.Write(x);
		out.Write(y);
		out.Write(z);
	}
};

// A warmed world. One iteration measures one server tick
// after construction has been emitted for every client and the serializer is warm.
template <bool useDirtyList, bool bulkFallback = false>
void MeasureLargeWorld(benchmark::State &state)
{
	const int clients = static_cast<int>(state.range(0));
	const int changed = static_cast<int>(state.range(1));
	SerializationCandidates candidates;
	candidates.fullScan = true;
	if constexpr (useDirtyList && !bulkFallback)
		candidates.changed.reserve(changed);
	World<PositionReplica> world(clients, 50000, useDirtyList ? &candidates : nullptr);
	world.Tick();
	world.Tick();
	world.peer.bytes = 0;
	world.peer.messages = 0;
	candidates.fullScan = bulkFallback;
	for (auto _ : state)
	{
		if constexpr (useDirtyList && !bulkFallback)
			candidates.changed.clear();
		for (int i = 0; i < changed; ++i)
		{
			world.replicas[i].x += 1.0f;
			if constexpr (useDirtyList && !bulkFallback)
				candidates.changed.push_back(&world.replicas[i]);
		}
		world.Tick();
	}
	if (world.peer.messages != uint64_t(clients) * changed * state.iterations())
		state.SkipWithError("unexpected number of position updates");
	state.counters["rm3_messages_per_tick"] = benchmark::Counter(world.peer.messages, benchmark::Counter::kAvgIterations);
	state.counters["rm3_bytes_per_tick"] = benchmark::Counter(world.peer.bytes, benchmark::Counter::kAvgIterations);
}
BENCHMARK_TEMPLATE(MeasureLargeWorld, false)->Name("ReplicaManager3_LargeWorld")->ArgsProduct({{1, 20}, {0, 500, 50000}})->ArgNames({"clients", "changed_of_50000"})->Iterations(20);
BENCHMARK_TEMPLATE(MeasureLargeWorld, true)->Name("ReplicaManager3_LargeWorldDirtyList")->ArgsProduct({{1, 20}, {0, 500, 50000}})->ArgNames({"clients", "changed_of_50000"})->Iterations(20);
BENCHMARK_TEMPLATE(MeasureLargeWorld, true, true)->Name("ReplicaManager3_LargeWorldBulkFallback")->ArgsProduct({{1, 20}, {50000}})->ArgNames({"clients", "changed_of_50000"})->Iterations(20);

}
