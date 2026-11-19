/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <functional>
#include <utility>
#include <vector>

#include "SimulatedNetwork.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/replica_manager3.h"

using namespace MafiaNet;
namespace
{
class Field : public Replica3
{
public:
	explicit Field(bool sender = true)
	    : sender(sender) {}
	bool BroadcastsIdentically() const override { return shared; }
	void WriteAllocationID(Connection_RM3 *, BitStream *) const override {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *to, ReplicaManager3 *) override
	{
		if (!sender)
			return RM3CS_NEVER_CONSTRUCT;
		return to->GetRakNetGUID() == hiddenFrom ? RM3CS_NO_ACTION : RM3CS_SEND_CONSTRUCTION;
	}
	RM3DestructionState QueryDestruction(Connection_RM3 *to, ReplicaManager3 *) override
	{
		return to->GetRakNetGUID() == hiddenFrom ? RM3DS_SEND_DESTRUCTION : RM3DS_NO_ACTION;
	}
	bool QueryRemoteConstruction(Connection_RM3 *) override { return !sender; }
	void SerializeConstruction(BitStream *out, Connection_RM3 *to) override { out->Write(ValueFor(to)); }
	bool DeserializeConstruction(BitStream *in, Connection_RM3 *) override { return in->Read(value); }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return true; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return sender ? RM3AOPC_DO_NOTHING : RM3AOPC_DELETE_REPLICA; }
	void DeallocReplica(Connection_RM3 *) override { delete this; }
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *to) override
	{
		if (!sender)
			return RM3QSR_NEVER_CALL_SERIALIZE;
		return to->GetRakNetGUID() == deferredFor ? RM3QSR_DO_NOT_CALL_SERIALIZE : RM3QSR_CALL_SERIALIZE;
	}
	RM3SerializationResult Serialize(SerializeParameters *parameters) override
	{
		parameters->pro[0].reliability = reliability;
		parameters->outputBitstream[0].Write(ValueFor(parameters->destinationConnection));
		if (afterWrite)
		{
			auto once = std::move(afterWrite);
			afterWrite = nullptr;
			once();
		}
		if (forceRefresh)
			return RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION;
		return shared ? RM3SR_BROADCAST_IDENTICALLY : RM3SR_SERIALIZED_UNIQUELY;
	}
	void Deserialize(DeserializeParameters *parameters) override
	{
		ASSERT_TRUE(parameters->serializationBitstream[0].Read(value));
	}

	bool shared = true;
	bool forceRefresh = false;
	Reliability reliability = Reliability::ReliableOrdered;
	int value = 1;
	int bonus = 0;
	RakNetGUID bonusFor = UNASSIGNED_RAKNET_GUID;
	RakNetGUID hiddenFrom = UNASSIGNED_RAKNET_GUID;
	RakNetGUID deferredFor = UNASSIGNED_RAKNET_GUID;
	std::function<void()> afterWrite;

private:
	int ValueFor(Connection_RM3 *to) const { return value + (to && to->GetRakNetGUID() == bonusFor ? bonus : 0); }
	const bool sender;
};

struct Candidates
{
	std::vector<Replica3 *> pending, serving;
	bool enabled = false;
	bool bulk = false;
};

class Connection : public Connection_RM3
{
public:
	Connection(const SystemAddress &address, RakNetGUID guid, const Candidates &candidates)
	    : Connection_RM3(address, guid), candidates(candidates) {}
	ConstructionMode QueryConstructionMode() const override { return QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION; }
	Replica3 *AllocReplica(BitStream *, ReplicaManager3 *) override { return new Field(false); }
	bool QuerySerializationList(std::vector<Replica3 *> &out) override
	{
		if (!candidates.enabled || candidates.bulk)
		{
			personal.clear();
			return false;
		}
		out = candidates.serving;
		out.insert(out.end(), personal.begin(), personal.end());
		personal.clear();
		return true;
	}
	std::vector<Replica3 *> personal;

private:
	const Candidates &candidates;
};

class Endpoint : public ReplicaManager3
{
public:
	explicit Endpoint(RakPeer *peer)
	    : peer(peer)
	{
		peer->AttachPlugin(this);
		SetNetworkIDManager(&ids);
		SetAutoSerializeInterval(0);
	}
	~Endpoint() override { peer->DetachPlugin(this); }
	void Update() override
	{
		candidates.serving.swap(candidates.pending);
		ReplicaManager3::Update();
		candidates.serving.clear();
		candidates.bulk = false;
	}
	Connection_RM3 *AllocConnection(const SystemAddress &address, RakNetGUID guid) const override
	{
		return new Connection(address, guid, candidates);
	}
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }
	int Read(Field &field)
	{
		const auto *copy = ids.GET_OBJECT_FROM_ID<Field *>(field.GetNetworkID());
		return copy ? copy->value : -1;
	}

	RakPeer *const peer;
	Candidates candidates;
	NetworkIDManager ids;
};

class DirtyListDelivery : public ::testing::TestWithParam<bool>
{
};
} // namespace

TEST_P(DirtyListDelivery, ReliableStateSurvivesLossAndScopeChanges)
{
	SimulatedNetwork net(827);
	Endpoint server(net.AddPeer(2, 2)), first(net.AddPeer(1)), late(net.AddPeer(1));
	server.candidates.enabled = GetParam();
	Field position, personal;
	personal.shared = false;
	server.Reference(&position);
	server.Reference(&personal);
	ASSERT_TRUE(net.Connect(first.peer, server.peer));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return first.Read(position) == 1 && first.Read(personal) == 1; }, 5000));

	// Queue just one mutation while every outgoing datagram to this observer is dropped.
	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetLink(server.peer, first.peer, dark);
	const auto dropped = net.DatagramsDropped();
	position.value = 2;
	server.candidates.pending.push_back(&position);
	net.Step(100);
	EXPECT_GT(net.DatagramsDropped(), dropped);
	EXPECT_EQ(first.Read(position), 1);
	net.SetLink(server.peer, first.peer, {});
	ASSERT_TRUE(net.RunUntil([&]
	                         { return first.Read(position) == 2; }, 5000));

	// A new observer has no dirty event; construction must carry the current state.
	ASSERT_TRUE(net.Connect(late.peer, server.peer));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return late.Read(position) == 2 && late.Read(personal) == 1; }, 5000));
	position.hiddenFrom = late.peer->GetMyGUID();
	ASSERT_TRUE(net.RunUntil([&]
	                         { return late.Read(position) == -1; }, 5000));

	// A write made while serializing belongs to the next pending set.
	position.value = 3;
	position.afterWrite = [&]
	{
		personal.value = 4;
		server.candidates.pending.push_back(&personal);
	};
	server.candidates.pending.push_back(&position);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return first.Read(position) == 3 && first.Read(personal) == 4 && late.Read(personal) == 4; }, 5000));
	EXPECT_EQ(late.Read(position), -1);
	position.hiddenFrom = UNASSIGNED_RAKNET_GUID;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return late.Read(position) == 3; }, 5000));

	// Only this recipient's representation changed; the global queue stays empty.
	personal.bonus = 10;
	personal.bonusFor = late.peer->GetMyGUID();
	auto *connection = static_cast<Connection *>(server.GetConnectionByGUID(personal.bonusFor));
	ASSERT_NE(connection, nullptr);
	connection->personal.push_back(&personal);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return late.Read(personal) == 14; }, 5000));
	EXPECT_EQ(first.Read(personal), 4);

	// A receiver deferred by a gameplay budget needs a candidate when it becomes eligible again.
	personal.deferredFor = late.peer->GetMyGUID();
	personal.value = 7;
	server.candidates.pending.push_back(&personal);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return first.Read(personal) == 7; }, 5000));
	EXPECT_EQ(late.Read(personal), 14);
	personal.deferredFor = UNASSIGNED_RAKNET_GUID;
	connection->personal.push_back(&personal);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return late.Read(personal) == 17; }, 5000));

	// A known bulk rewrite falls back to the full scan, without collecting individual marks.
	position.value = 5;
	personal.value = 6;
	server.candidates.bulk = true;
	ASSERT_TRUE(net.RunUntil([&]
	                         { return first.Read(position) == 5 && late.Read(position) == 5 && first.Read(personal) == 6 && late.Read(personal) == 16; }, 5000));
}
TEST_P(DirtyListDelivery, ScheduledRefreshRestoresALostUnreliablePosition)
{
	SimulatedNetwork net(8123);
	Endpoint server(net.AddPeer(1, 1)), observer(net.AddPeer(1));
	server.candidates.enabled = GetParam();
	Field position;
	position.reliability = Reliability::Unreliable;
	server.Reference(&position);
	ASSERT_TRUE(net.Connect(observer.peer, server.peer));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return observer.Read(position) == 1; }, 5000));

	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetLink(server.peer, observer.peer, dark);
	const auto dropped = net.DatagramsDropped();
	position.value = 9;
	server.candidates.pending.push_back(&position);
	net.Step(100);
	EXPECT_GT(net.DatagramsDropped(), dropped);
	net.SetLink(server.peer, observer.peer, {});
	net.Step(100);
	EXPECT_EQ(observer.Read(position), 1);

	// The sender already compared these exact bytes. A scheduled force refresh carries them again.
	position.forceRefresh = true;
	server.candidates.pending.push_back(&position);
	EXPECT_TRUE(net.RunUntil([&]
	                         { return observer.Read(position) == 9; }, 5000));
}

INSTANTIATE_TEST_SUITE_P(DefaultAndDirty, DirtyListDelivery, ::testing::Bool());
