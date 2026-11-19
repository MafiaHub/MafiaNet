/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>

#include "mafianet/replica_manager3.h"

#if _RAKNET_SUPPORT_ReplicaManager3 == 1
#include <array>
#include "mafianet/network_id_manager.h"
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
		bytes += data->GetNumberOfBytesUsed();
		return 1;
	}
	uint64_t bytes = 0;
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

struct World
{
	CountingPeer peer;
	NetworkIDManager ids;
	Manager manager;
	std::array<StateReplica, 100> replicas;
	explicit World(int clients)
	{
		tickUS = 1000000;
		SetTimeSourceOverride(TickClock);
		peer.AttachPlugin(&manager);
		manager.SetNetworkIDManager(&ids);
		manager.SetAutoSerializeInterval(0);
		for (int i = 0; i < clients; ++i)
			manager.PushConnection(new Connection(RakNetGUID(100 + i)));
		for (auto &replica : replicas)
			manager.Reference(&replica);
	}
	~World()
	{
		peer.DetachPlugin(&manager);
		SetTimeSourceOverride(nullptr);
	}
	void Tick()
	{
		tickUS += 16000;
		manager.Update();
	}
};

void ReplicaManager3_Construct(benchmark::State &state)
{
	uint64_t total = 0;
	for (auto _ : state)
	{
		state.PauseTiming();
		{
			World world(static_cast<int>(state.range(0)));
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
BENCHMARK(ReplicaManager3_Construct)->ArgName("clients")->Arg(1)->Arg(8)->Iterations(32);

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
}
#endif
