// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/replica_manager3.h"
#include <utility>

namespace MafiaNet
{
/// A complete state value with Write(BitStream&) const and bool Read(BitStream&).
/// Decoding into a temporary keeps the last good state after an incomplete snapshot.
template <class State>
class SnapshotReplica : public Replica3
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
			ReadSnapshot(parameters->serializationBitstream[0]);
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
}
