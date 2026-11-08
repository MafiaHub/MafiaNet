/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// The serialize pass of a server with many connections and replicas that broadcast identically: one
// set of bytes per replica, sent to every connection that holds it.

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"
#include "mafianet/replica_manager3.h"

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace MafiaNet;

namespace
{
std::atomic<TimeUS> g_passNowUS{1000000};
TimeUS PassClock()
{
	return g_passNowUS.load();
}

class PassReplica : public Replica3
{
public:
	explicit PassReplica(bool broadcastsIdentically)
	    : broadcastsIdentically(broadcastsIdentically)
	{
	}

	// The opt-in; plain (not `override`) so the file also builds against a ReplicaManager3 without it.
	bool BroadcastsIdentically() const { return broadcastsIdentically; }

	void WriteAllocationID(Connection_RM3 *, MafiaNet::BitStream *) const {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) { return RM3CS_SEND_CONSTRUCTION; }
	bool QueryRemoteConstruction(Connection_RM3 *) { return false; }
	void SerializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) {}
	bool DeserializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
	void SerializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) {}
	bool DeserializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) {}
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) { return RM3QSR_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) {}

	RM3SerializationResult Serialize(SerializeParameters *parameters)
	{
		++serializeCalls;
		{
			std::lock_guard<std::mutex> lock(threadsMutex);
			serializeThreads.insert(std::this_thread::get_id());
		}
		parameters->outputBitstream[0].Write(value);
		return RM3SR_BROADCAST_IDENTICALLY;
	}

	const bool broadcastsIdentically;
	int value = 1;
	std::atomic<int> serializeCalls{0};
	std::mutex threadsMutex;
	std::set<std::thread::id> serializeThreads;
};

// Wants every replica it is given, and records what reaches SendSerialize.
class PassConnection : public Connection_RM3
{
public:
	explicit PassConnection(RakNetGUID guid)
	    : Connection_RM3(UNASSIGNED_SYSTEM_ADDRESS, guid)
	{
		isValidated = true;
	}

	Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
	ConstructionMode QueryConstructionMode() const { return QUERY_CONNECTION_FOR_REPLICA_LIST; }

	void QueryReplicaList(DataStructures::List<Replica3 *> &create, DataStructures::List<Replica3 *> &)
	{
		for (Replica3 *replica : wanted)
		{
			if (!HasReplicaConstructed(replica))
				create.Push(replica, _FILE_AND_LINE_);
		}
	}

	SendSerializeIfChangedResult SendSerialize(Replica3 *replica, bool indicesToSend[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], MafiaNet::BitStream serializationData[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], Time timestamp, PRO sendParameters[RM3_NUM_OUTPUT_BITSTREAM_CHANNELS], RakPeerInterface *rakPeer, unsigned char worldId, Time curTime)
	{
		++sendCalls;
		if (indicesToSend[0] && serializationData[0].GetNumberOfBitsUsed() > 0)
		{
			MafiaNet::BitStream copy(serializationData[0].GetData(), serializationData[0].GetNumberOfBytesUsed(), false);
			int value = 0;
			copy.Read(value);
			received.push_back(value);
		}
		threads.insert(std::this_thread::get_id());
		return Connection_RM3::SendSerialize(replica, indicesToSend, serializationData, timestamp, sendParameters, rakPeer, worldId, curTime);
	}

	std::vector<Replica3 *> wanted;
	int sendCalls = 0;
	std::vector<int> received;
	std::set<std::thread::id> threads;
};

class PassManager : public ReplicaManager3
{
public:
	Connection_RM3 *AllocConnection(const SystemAddress &, RakNetGUID guid) const { return new PassConnection(guid); }
	void DeallocConnection(Connection_RM3 *connection) const { delete connection; }
};

class ReplicaManager3SerializePass : public ::testing::Test
{
protected:
	void SetUp() override
	{
		SetTimeSourceOverride(&PassClock);
		peer = RakPeerInterface::GetInstance();
		peer->AttachPlugin(&manager);
		manager.SetNetworkIDManager(&ids);
		manager.SetAutoSerializeInterval(0);
	}

	void TearDown() override
	{
		for (PassReplica *replica : replicas)
		{
			manager.Dereference(replica);
			delete replica;
		}
		peer->DetachPlugin(&manager);
		RakPeerInterface::DestroyInstance(peer);
		SetTimeSourceOverride(nullptr);
	}

	void Tick()
	{
		g_passNowUS += 10000;
		manager.Update();
	}

	PassReplica *AddReplica(bool broadcastsIdentically)
	{
		auto *replica = new PassReplica(broadcastsIdentically);
		replicas.push_back(replica);
		manager.Reference(replica);
		return replica;
	}

	std::vector<PassConnection *> AddConnections(int count, const std::vector<Replica3 *> &wanted)
	{
		std::vector<PassConnection *> added;
		for (int i = 0; i < count; i++)
		{
			auto *connection = static_cast<PassConnection *>(manager.AllocConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(100 + static_cast<uint64_t>(i))));
			connection->wanted = wanted;
			manager.PushConnection(connection);
			added.push_back(connection);
		}
		return added;
	}

	NetworkIDManager ids;
	PassManager manager;
	RakPeerInterface *peer = nullptr;
	std::vector<PassReplica *> replicas;
};
} // namespace

// An unchanged replica has nothing to send to anyone. On a server that is most replicas most ticks,
// and handing each of them to every connection's SendSerialize to find that out costs
// connections x replicas calls a tick for nothing.
TEST_F(ReplicaManager3SerializePass, IdleReplicaCostsItsConnectionsNothing)
{
	PassReplica *replica = AddReplica(true);
	std::vector<PassConnection *> connections = AddConnections(8, {replica});
	Tick(); // construct
	Tick(); // first state

	for (PassConnection *connection : connections)
		connection->sendCalls = 0;
	Tick();
	int calls = 0;
	for (PassConnection *connection : connections)
		calls += connection->sendCalls;
	EXPECT_EQ(calls, 0) << "an idle replica was handed to SendSerialize for every connection";
}
