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

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
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
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) { return refuse ? RM3QSR_DO_NOT_CALL_SERIALIZE : RM3QSR_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) {}

	RM3SerializationResult Serialize(SerializeParameters *parameters)
	{
		if (onSerialize)
		{
			std::function<void()> once = std::move(onSerialize);
			onSerialize = nullptr;
			once();
		}
		++serializeCalls;
		{
			std::lock_guard<std::mutex> lock(threadsMutex);
			serializeThreads.insert(std::this_thread::get_id());
		}
		parameters->outputBitstream[0].Write(value);
		return RM3SR_BROADCAST_IDENTICALLY;
	}

	const bool broadcastsIdentically;
	// Every connection is told not to serialize it, while set.
	bool refuse = false;
	// Run once, from inside the next Serialize(): what a game callback might do mid-pass.
	std::function<void()> onSerialize;
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

namespace
{
// Spreads the iterations over real threads, the way an application's pool would.
class ThreadExecutor : public RM3SerializeExecutor
{
public:
	void ParallelFor(unsigned int count, void (*fn)(void *context, unsigned int index), void *context) override
	{
		++runs;
		std::atomic<unsigned int> next{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++)
		{
			threads.emplace_back([&]
			                     {
				for (unsigned int i = next++; i < count; i = next++)
					fn(context, i); });
		}
		for (std::thread &thread : threads)
			thread.join();
	}
	int runs = 0;
};
} // namespace

// A change is serialized once and the same bytes reach every connection on the tick it happens.
TEST_F(ReplicaManager3SerializePass, ChangedReplicaIsSerializedOnceForEveryConnection)
{
	PassReplica *replica = AddReplica(true);
	std::vector<PassConnection *> connections = AddConnections(8, {replica});
	Tick();
	Tick();

	replica->value = 7;
	replica->serializeCalls = 0;
	for (PassConnection *connection : connections)
		connection->received.clear();
	Tick();
	EXPECT_EQ(replica->serializeCalls.load(), 1);
	for (PassConnection *connection : connections)
		EXPECT_EQ(connection->received, std::vector<int>{7});
}

// Served across an executor, every connection gets what a pass on one thread gives it.
TEST_F(ReplicaManager3SerializePass, ExecutorServesEveryConnectionWhatOneThreadWould)
{
	ThreadExecutor executor;
	manager.SetSerializeExecutor(&executor);
	std::vector<Replica3 *> wanted;
	std::vector<PassReplica *> broadcasts;
	for (int i = 0; i < 16; i++)
	{
		broadcasts.push_back(AddReplica(true));
		wanted.push_back(broadcasts.back());
	}
	std::vector<PassConnection *> connections = AddConnections(32, wanted);
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	for (int tick = 0; tick < 5; tick++)
	{
		for (int i = 0; i < 16; i++)
		{
			// Half of them change each tick.
			if ((i + tick) % 2 == 0)
				broadcasts[static_cast<size_t>(i)]->value = 100 * tick + i;
		}
		Tick();
	}
	EXPECT_GT(executor.runs, 0);

	std::vector<int> expected;
	for (int tick = 0; tick < 5; tick++)
	{
		for (int i = 0; i < 16; i++)
		{
			if ((i + tick) % 2 == 0)
				expected.push_back(100 * tick + i);
		}
	}
	for (PassConnection *connection : connections)
	{
		std::vector<int> got = connection->received;
		std::sort(got.begin(), got.end());
		std::vector<int> want = expected;
		std::sort(want.begin(), want.end());
		EXPECT_EQ(got, want);
	}
	for (PassReplica *replica : broadcasts)
		EXPECT_EQ(replica->serializeThreads.size(), 1u) << "a broadcast was serialized off the calling thread";
	manager.SetSerializeExecutor(0);
}

// A replica that has not opted in is serialized per connection, on the thread running Update().
TEST_F(ReplicaManager3SerializePass, ReplicaThatDoesNotOptInStaysOnTheCallingThread)
{
	ThreadExecutor executor;
	manager.SetSerializeExecutor(&executor);
	PassReplica *plain = AddReplica(false);
	PassReplica *broadcast = AddReplica(true);
	std::vector<PassConnection *> connections = AddConnections(8, {plain, broadcast});
	Tick(); // construction sends each its first state
	for (PassConnection *connection : connections)
		connection->received.clear();
	plain->value = 3;
	broadcast->value = 4;
	Tick();
	EXPECT_EQ(plain->serializeThreads.size(), 1u);
	EXPECT_EQ(*plain->serializeThreads.begin(), std::this_thread::get_id());
	for (PassConnection *connection : connections)
	{
		std::vector<int> got = connection->received;
		std::sort(got.begin(), got.end());
		EXPECT_EQ(got, (std::vector<int>{3, 4}));
	}
	manager.SetSerializeExecutor(0);
}

// A pass in which every connection holding a broadcast declines it must not consume its change: on the
// pass they accept it again, each of them still gets it. Serialized ahead of an executor, the replica
// used to be prepared anyway, its new bytes recorded as sent to nobody.
TEST_F(ReplicaManager3SerializePass, ChangeDeclinedByEveryConnectionIsStillDeliveredLater)
{
	ThreadExecutor executor;
	manager.SetSerializeExecutor(&executor);
	PassReplica *replica = AddReplica(true);
	std::vector<PassConnection *> connections = AddConnections(4, {replica});
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	replica->value = 9;
	replica->refuse = true;
	Tick();
	replica->refuse = false;
	Tick();
	for (PassConnection *connection : connections)
		EXPECT_EQ(connection->received, std::vector<int>{9});
	manager.SetSerializeExecutor(0);
}

// A broadcast the up-front loop misses (here because a Serialize() there dereferences a replica ahead
// of it, shifting the list) is prepared later by whichever connection reaches it first. Every other
// connection must still be served it, not take it for one the workers already handled.
TEST_F(ReplicaManager3SerializePass, BroadcastPreparedLateReachesEveryConnection)
{
	ThreadExecutor executor;
	manager.SetSerializeExecutor(&executor);
	PassReplica *dropped = AddReplica(true);
	PassReplica *dropper = AddReplica(true);
	PassReplica *late = AddReplica(true);
	std::vector<PassConnection *> connections = AddConnections(4, {dropped, dropper, late});
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	dropper->value = 5;
	late->value = 6;
	dropper->onSerialize = [&]
	{
		manager.Dereference(dropped);
	};
	Tick();
	for (PassConnection *connection : connections)
	{
		std::vector<int> got = connection->received;
		std::sort(got.begin(), got.end());
		EXPECT_EQ(got, (std::vector<int>{5, 6}));
	}
	manager.SetSerializeExecutor(0);
}

// A game callback may drop a connection in the middle of the pass, as a kick does. The pass must not
// go on to serve the freed connection.
TEST_F(ReplicaManager3SerializePass, ConnectionDroppedMidPassIsNotServed)
{
	ThreadExecutor executor;
	manager.SetSerializeExecutor(&executor);
	PassReplica *replica = AddReplica(true);
	std::vector<PassConnection *> connections = AddConnections(4, {replica});
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	PassConnection *kicked = connections.back();
	connections.pop_back();
	replica->value = 3;
	replica->onSerialize = [&]
	{
		Connection_RM3 *popped = manager.PopConnection(kicked->GetRakNetGUID());
		manager.DeallocConnection(popped);
	};
	Tick();
	for (PassConnection *connection : connections)
		EXPECT_EQ(connection->received, std::vector<int>{3});
	manager.SetSerializeExecutor(0);
}

// A connection that picks its own list is served whole on the calling thread, before the executor runs,
// and only what it holds: a replica it names but does not hold is skipped, not looked up through another
// connection's record. Every other connection still gets each broadcast, once.
TEST_F(ReplicaManager3SerializePass, OwnListConnectionIsServedOnTheCallingThreadOnly)
{
	struct OwnListConnection : PassConnection
	{
		using PassConnection::PassConnection;
		bool QuerySerializationList(DataStructures::List<Replica3 *> &replicasToSerialize)
		{
			for (Replica3 *replica : list)
				replicasToSerialize.Push(replica, _FILE_AND_LINE_);
			return true;
		}
		std::vector<Replica3 *> list;
	};

	ThreadExecutor executor;
	manager.SetSerializeExecutor(&executor);
	PassReplica *held = AddReplica(true);
	PassReplica *notHeld = AddReplica(true);
	std::vector<PassConnection *> others = AddConnections(4, {held, notHeld});
	auto *picker = new OwnListConnection(RakNetGUID(200));
	picker->wanted = {held};
	picker->list = {held, notHeld};
	manager.PushConnection(picker);
	Tick();
	Tick();
	ASSERT_FALSE(picker->HasReplicaConstructed(notHeld));
	for (PassConnection *connection : others)
		connection->received.clear();
	picker->received.clear();
	picker->threads.clear();

	held->value = 11;
	notHeld->value = 12;
	held->serializeCalls = 0;
	notHeld->serializeCalls = 0;
	Tick();
	EXPECT_EQ(picker->received, std::vector<int>{11});
	EXPECT_EQ(picker->threads, std::set<std::thread::id>{std::this_thread::get_id()});
	for (PassConnection *connection : others)
	{
		std::vector<int> got = connection->received;
		std::sort(got.begin(), got.end());
		EXPECT_EQ(got, (std::vector<int>{11, 12}));
	}
	EXPECT_EQ(held->serializeCalls.load(), 1);
	EXPECT_EQ(notHeld->serializeCalls.load(), 1);
	manager.SetSerializeExecutor(0);
}

// What a game callback may do in the middle of a pass on the calling thread: dereference the replica it is
// serializing, drop a connection, run Update() again. The pass must neither touch what that freed nor lose
// the rest of the tick's updates.
namespace
{
// Broadcast or per-connection replicas, with or without an executor.
class SerializePassCallbacks : public ReplicaManager3SerializePass, public ::testing::WithParamInterface<std::tuple<bool, bool>>
{
protected:
	void SetUp() override
	{
		ReplicaManager3SerializePass::SetUp();
		if (std::get<1>(GetParam()))
			manager.SetSerializeExecutor(&executor);
	}
	void TearDown() override
	{
		manager.SetSerializeExecutor(0);
		ReplicaManager3SerializePass::TearDown();
	}
	bool Broadcasts() const { return std::get<0>(GetParam()); }

	ThreadExecutor executor;
};
} // namespace

// A replica that leaves the world from its own Serialize() takes its record with it on every connection.
// The connection being served must not use that record afterwards, nor skip the replica that took its slot.
TEST_P(SerializePassCallbacks, ReplicaDereferencedFromItsOwnSerializeLosesNoOtherUpdate)
{
	PassReplica *leaving = AddReplica(Broadcasts());
	PassReplica *next = AddReplica(Broadcasts());
	std::vector<PassConnection *> connections = AddConnections(3, {leaving, next});
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	leaving->value = 4;
	next->value = 8;
	leaving->onSerialize = [&]
	{
		manager.Dereference(leaving);
	};
	Tick();
	for (PassConnection *connection : connections)
	{
		const std::vector<int> &got = connection->received;
		EXPECT_EQ(std::count(got.begin(), got.end(), 8), 1) << "the replica after the one that left was skipped";
	}
}

// A callback that drops the very connection being served (a kick) must end that connection's share of the
// pass, not carry on through its freed lists.
TEST_P(SerializePassCallbacks, ConnectionDroppedWhileBeingServedIsLeftAlone)
{
	PassReplica *first = AddReplica(Broadcasts());
	PassReplica *second = AddReplica(Broadcasts());
	std::vector<PassConnection *> connections = AddConnections(3, {first, second});
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	PassConnection *kicked = connections.front();
	connections.erase(connections.begin());
	first->value = 2;
	second->value = 3;
	first->onSerialize = [&]
	{
		manager.DeallocConnection(manager.PopConnection(kicked->GetRakNetGUID()));
	};
	Tick();
	for (PassConnection *connection : connections)
	{
		std::vector<int> got = connection->received;
		std::sort(got.begin(), got.end());
		EXPECT_EQ(got, (std::vector<int>{2, 3}));
	}
}

// Update() run again from inside a pass (directly, or through RakPeer::Receive) must not start a second pass
// inside the first: on POSIX it locks a mutex the pass already holds, and anywhere it would overwrite the
// pass's state. It returns at once instead.
TEST_P(SerializePassCallbacks, UpdateRunFromInsideAPassReturnsAtOnce)
{
	PassReplica *reentering = AddReplica(Broadcasts());
	PassReplica *other = AddReplica(Broadcasts());
	std::vector<PassConnection *> connections = AddConnections(3, {reentering, other});
	Tick();
	Tick();
	for (PassConnection *connection : connections)
		connection->received.clear();

	reentering->value = 5;
	other->value = 6;
	reentering->onSerialize = [&]
	{
		g_passNowUS += 10000;
		manager.Update();
	};
	Tick();
	for (PassConnection *connection : connections)
	{
		std::vector<int> got = connection->received;
		std::sort(got.begin(), got.end());
		EXPECT_EQ(got, (std::vector<int>{5, 6}));
	}
}

INSTANTIATE_TEST_SUITE_P(Passes, SerializePassCallbacks, ::testing::Combine(::testing::Bool(), ::testing::Bool()), [](const ::testing::TestParamInfo<std::tuple<bool, bool>> &info)
                         { return std::string(std::get<0>(info.param) ? "Broadcast" : "PerConnection") + (std::get<1>(info.param) ? "Parallel" : "Serial"); });
