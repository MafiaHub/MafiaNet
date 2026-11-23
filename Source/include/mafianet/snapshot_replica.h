// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/replica_manager3.h"
#include "mafianet/replication_manager.h"
#include <utility>
#include <algorithm>
#include <deque>
#include <iterator>

namespace MafiaNet
{
/// A complete state value with Write(BitStream&) const and bool Read(BitStream&).
/// Decoding into a temporary keeps the last good state after an incomplete snapshot.
template <class State, std::size_t HistoryCapacity = 0>
class SnapshotReplica;

template <class State>
class SnapshotReplica<State, 0> : public Replica3, public SnapshotReplicaTag
{
public:
	State state{};
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_SEND_CONSTRUCTION; }
	bool QueryRemoteConstruction(Connection_RM3 *) override { return true; }
	void SerializeConstruction(BitStream *out, Connection_RM3 *) override { state.Write(*out); }
	bool DeserializeConstruction(BitStream *in, Connection_RM3 *) override { return ReadSnapshot(*in); }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return true; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) override { delete this; }
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) override { return RM3QSR_CALL_SERIALIZE; }
	bool BroadcastsIdentically() const override { return true; }
	RM3SerializationResult Serialize(SerializeParameters *parameters) override
	{
		state.Write(parameters->outputBitstream[0]);
		return RM3SR_BROADCAST_IDENTICALLY;
	}
	void Deserialize(DeserializeParameters *parameters) override
	{
		if (parameters->bitstreamWrittenTo[0])
			parameters->bitstreamWrittenTo[0] = ReadSnapshot(parameters->serializationBitstream[0]);
	}

private:
	bool ReadSnapshot(BitStream &in)
	{
		State next{};
		if (!next.Read(in) || in.GetNumberOfUnreadBits() != 0)
			return false;
		state = std::move(next);
		return true;
	}
};

/// Opt into a tick-indexed history with SnapshotReplica<State, N> on both peers.
template <class State, std::size_t HistoryCapacity>
class SnapshotReplica : public SnapshotReplica<State, 0>
{
	static_assert(HistoryCapacity >= 2, "interpolation requires at least two samples");
	struct Sample
	{
		Time tick;
		State value;
	};
	std::deque<Sample> history;
	uint32_t discontinuity = 0;
	Time SenderTick() const { return this->replicaManager ? this->replicaManager->GetSimulationTick() : 0; }
	bool ReceiveSample(BitStream &in, Time tick, bool baseline)
	{
		State next{};
		uint32_t generation;
		if (!in.Read(generation) || !next.Read(in) || in.GetNumberOfUnreadBits())
			return false;
		if (!baseline && !history.empty() && tick < history.back().tick)
			return false;
		if (baseline || discontinuity != generation)
			history.clear();
		if (!history.empty() && tick == history.back().tick)
			history.pop_back();
		if (history.size() == HistoryCapacity)
			history.pop_front();
		discontinuity = generation;
		this->state = std::move(next);
		history.push_back({tick, this->state});
		return true;
	}
	void WriteSample(BitStream &out) const
	{
		out.Write(discontinuity);
		this->state.Write(out);
	}

public:
	static constexpr std::size_t SnapshotHistoryCapacity = HistoryCapacity;
	void ResetSnapshotHistory() override { history.clear(); }
	void MarkTeleport() { ++discontinuity; }
	Time LatestSnapshotTick() const { return history.empty() ? 0 : history.back().tick; }
	template <class Interpolate>
	State SampleAt(double tick, Interpolate interpolate) const
	{
		if (history.empty())
			return this->state;
		auto upper = std::lower_bound(history.begin(), history.end(), tick, [](const Sample &sample, double target)
		                              { return static_cast<double>(sample.tick) < target; });
		if (upper == history.end())
			return history.back().value;
		if (upper == history.begin() || tick == static_cast<double>(upper->tick))
			return upper->value;
		const auto &lower = *std::prev(upper);
		return interpolate(lower.value, upper->value, (tick - static_cast<double>(lower.tick)) / static_cast<double>(upper->tick - lower.tick));
	}
	void SerializeConstruction(BitStream *out, Connection_RM3 *) override
	{
		out->Write(SenderTick());
		WriteSample(*out);
	}
	bool DeserializeConstruction(BitStream *in, Connection_RM3 *) override
	{
		Time tick;
		return in->Read(tick) && ReceiveSample(*in, tick, true);
	}
	void SerializeConstructionExisting(BitStream *out, Connection_RM3 *c) override { SerializeConstruction(out, c); }
	void DeserializeConstructionExisting(BitStream *in, Connection_RM3 *c) override { DeserializeConstruction(in, c); }
	RM3SerializationResult Serialize(SerializeParameters *params) override
	{
		params->messageTimestamp = SenderTick();
		WriteSample(params->outputBitstream[0]);
		return RM3SR_BROADCAST_IDENTICALLY;
	}
	void Deserialize(DeserializeParameters *params) override
	{
		if (params->bitstreamWrittenTo[0])
			params->bitstreamWrittenTo[0] = ReceiveSample(params->serializationBitstream[0], params->timeStamp, false);
	}
};

/// Outstanding inputs for one local replica. Keep predicted and canonical state separate.
template <class Input, std::size_t Capacity = 64>
class OwnerInputHistory
{
	static_assert(Capacity > 0, "input history needs capacity");
	struct Entry
	{
		uint64_t sequence;
		Input input;
	};
	std::deque<Entry> pending;
	uint64_t epoch = 0;

public:
	void Clear()
	{
		pending.clear();
		epoch = 0;
	}
	std::size_t Size() const { return pending.size(); }
	bool Send(ReplicationManager &manager, Replica3 &object, const Input &input)
	{
		ReplicationManager::WorldGuard guard(manager);
		const auto current = manager.GetLocalInputStamp(object);
		if (epoch != current.epoch)
		{
			pending.clear();
			epoch = current.epoch;
		}
		if (!epoch || pending.size() == Capacity)
			return false;
		pending.push_back({0, input});
		OwnerInputStamp sent;
		if (!manager.SendOwnerUpdate(object, input, sent))
		{
			pending.pop_back();
			return false;
		}
		pending.back().sequence = sent.sequence;
		return true;
	}
	template <class State, class Simulate>
	State Reconcile(OwnerInputStamp processed, State canonical, Simulate simulate)
	{
		if (processed.epoch != epoch)
			return canonical;
		while (!pending.empty() && pending.front().sequence <= processed.sequence)
			pending.pop_front();
		for (const auto &entry : pending)
			simulate(canonical, entry.input);
		return canonical;
	}
};
}
