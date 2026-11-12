/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// A server that receives on one thread while another runs ReplicaManager3's outbound pass: the
// receive thread loops WaitForPackets()/Receive(), which is where a connection opens, closes and
// delivers its replicas and their state, and the pass thread serializes to every connection, creates
// and destroys the server's own replicas and takes replicas away from under the state arriving for them.

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_handle.h"
#include "mafianet/replica_manager3.h"
#include "mafianet/sleep.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace MafiaNet;

namespace
{
const unsigned int MAX_CLIENTS = 2;
const unsigned int REPLICAS_PER_CLIENT = 64;
// Of each client's replicas, the last ones are those the server's pass thread destroys mid-run when asked to.
const unsigned int KEPT_PER_CLIENT = 48;
const unsigned int SERVER_REPLICAS = 16;

class MirroredReplica;

// What the test thread reads of the server, which only its own threads may touch.
struct ServerView
{
	std::atomic<uint32_t> counters[MAX_CLIENTS * REPLICAS_PER_CLIENT] = {};
	std::atomic<unsigned int> opened{0};
	std::atomic<unsigned int> closed{0};
	std::atomic<unsigned int> passes{0};
	std::atomic<unsigned int> connectionsSeenByPass{0};
	std::atomic<unsigned int> executorRuns{0};
	std::atomic<unsigned int> churned{0};
	std::atomic<unsigned int> culled{0};

	// Set before the threads start: whether the pass thread destroys the replicas past KEPT_PER_CLIENT.
	bool cull = false;
	// Those replicas, once state has arrived for them, for the pass thread to take.
	std::mutex doomedMutex;
	std::vector<MirroredReplica *> doomed;
};

// The client's replica: it owns the counter and serializes it upstream.
class OwnedReplica : public Replica3
{
public:
	void WriteAllocationID(Connection_RM3 *, BitStream *) const override {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_SEND_CONSTRUCTION; }
	bool QueryRemoteConstruction(Connection_RM3 *) override { return false; }
	void SerializeConstruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeConstruction(BitStream *, Connection_RM3 *) override { return false; }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return false; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) override {}
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) override { return RM3QSR_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) override {}

	RM3SerializationResult Serialize(SerializeParameters *parameters) override
	{
		parameters->outputBitstream[0].Write(index);
		parameters->outputBitstream[0].Write(counter);
		return RM3SR_SERIALIZED_ALWAYS;
	}

	uint32_t index = 0;
	uint32_t counter = 0;
};

// The server's copy of a client's replica, serialized back to that client alone. Its counter is a plain
// field on purpose: the receive thread writes it in Deserialize() and the pass thread reads it in
// Serialize() for the same connection, which ReplicaManager3 keeps apart.
class MirroredReplica : public Replica3
{
public:
	explicit MirroredReplica(ServerView *view)
	    : view(view)
	{
	}

	~MirroredReplica() override
	{
		if (doomed)
		{
			std::lock_guard<std::mutex> lock(view->doomedMutex);
			view->doomed.erase(std::remove(view->doomed.begin(), view->doomed.end(), this), view->doomed.end());
		}
	}

	void WriteAllocationID(Connection_RM3 *, BitStream *) const override {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_NEVER_CONSTRUCT; }
	bool QueryRemoteConstruction(Connection_RM3 *) override { return true; }
	void SerializeConstruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeConstruction(BitStream *, Connection_RM3 *) override { return true; }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return true; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return RM3AOPC_DELETE_REPLICA; }
	void DeallocReplica(Connection_RM3 *) override { delete this; }
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) override { return RM3QSR_CALL_SERIALIZE; }

	void Deserialize(DeserializeParameters *parameters) override
	{
		uint32_t index = 0;
		if (!parameters->serializationBitstream[0].Read(index) || !parameters->serializationBitstream[0].Read(counter) || index >= MAX_CLIENTS * REPLICAS_PER_CLIENT)
			return;
		view->counters[index].store(counter);
		if (view->cull && !doomed && index % REPLICAS_PER_CLIENT >= KEPT_PER_CLIENT)
		{
			doomed = true;
			std::lock_guard<std::mutex> lock(view->doomedMutex);
			view->doomed.push_back(this);
		}
	}

	RM3SerializationResult Serialize(SerializeParameters *) override
	{
		lastSerialized = counter;
		return RM3SR_DO_NOT_SERIALIZE;
	}

	ServerView *view;
	uint32_t counter = 0;
	uint32_t lastSerialized = 0;
	bool doomed = false;
};

// A replica of the server's own, which its pass thread creates, changes, serializes to every connection
// and destroys.
class ServerOwnedReplica : public Replica3
{
public:
	explicit ServerOwnedReplica(bool broadcastsIdentically)
	    : broadcastsIdentically(broadcastsIdentically)
	{
	}

	bool BroadcastsIdentically() const override { return broadcastsIdentically; }
	void WriteAllocationID(Connection_RM3 *, BitStream *) const override {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_SEND_CONSTRUCTION; }
	bool QueryRemoteConstruction(Connection_RM3 *) override { return false; }
	void SerializeConstruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeConstruction(BitStream *, Connection_RM3 *) override { return false; }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return false; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) override {}
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) override { return RM3QSR_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) override {}

	RM3SerializationResult Serialize(SerializeParameters *parameters) override
	{
		parameters->outputBitstream[0].Write(value);
		return RM3SR_BROADCAST_IDENTICALLY;
	}

	const bool broadcastsIdentically;
	uint32_t value = 0;
};

// What a client saw of the server's own replicas.
struct ClientStats
{
	unsigned int constructed = 0;
	unsigned int destroyed = 0;
	unsigned int updates = 0;
};

// A client's copy of one of the server's own replicas.
class ServerOwnedCopy : public Replica3
{
public:
	explicit ServerOwnedCopy(ClientStats *stats)
	    : stats(stats)
	{
		++stats->constructed;
	}

	~ServerOwnedCopy() override { ++stats->destroyed; }

	void WriteAllocationID(Connection_RM3 *, BitStream *) const override {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) override { return RM3CS_NEVER_CONSTRUCT; }
	bool QueryRemoteConstruction(Connection_RM3 *) override { return true; }
	void SerializeConstruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeConstruction(BitStream *, Connection_RM3 *) override { return true; }
	void SerializeDestruction(BitStream *, Connection_RM3 *) override {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) override { return true; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const override { return RM3AOPC_DELETE_REPLICA; }
	void DeallocReplica(Connection_RM3 *) override { delete this; }
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) override { return RM3QSR_NEVER_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) override { ++stats->updates; }
	RM3SerializationResult Serialize(SerializeParameters *) override { return RM3SR_DO_NOT_SERIALIZE; }

private:
	ClientStats *stats;
};

class ClientConnection : public Connection_RM3
{
public:
	ClientConnection(const SystemAddress &address, RakNetGUID guid, ClientStats *stats)
	    : Connection_RM3(address, guid), stats(stats)
	{
	}

	Replica3 *AllocReplica(BitStream *, ReplicaManager3 *) override { return new ServerOwnedCopy(stats); }

private:
	ClientStats *stats;
};

class ClientManager : public ReplicaManager3
{
public:
	explicit ClientManager(ClientStats *stats)
	    : stats(stats)
	{
	}

	// One pass for each turn of the client's loop, not one for each packet it receives.
	void Update() override {}
	void RunPass() { ReplicaManager3::Update(); }

	Connection_RM3 *AllocConnection(const SystemAddress &address, RakNetGUID guid) const override { return new ClientConnection(address, guid, stats); }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }

private:
	ClientStats *stats;
};

class ServerConnection : public Connection_RM3
{
public:
	ServerConnection(const SystemAddress &address, RakNetGUID guid, ServerView *view)
	    : Connection_RM3(address, guid), view(view)
	{
	}

	Replica3 *AllocReplica(BitStream *, ReplicaManager3 *) override { return new MirroredReplica(view); }

private:
	ServerView *view;
};

// Serves the connections on two threads, as an application's pool would.
class TwoThreadExecutor : public RM3SerializeExecutor
{
public:
	explicit TwoThreadExecutor(ServerView *view)
	    : view(view)
	{
	}

	void ParallelFor(unsigned int count, void (*fn)(void *context, unsigned int index), void *context) override
	{
		++view->executorRuns;
		std::atomic<unsigned int> next{0};
		const auto work = [&]()
		{
			for (unsigned int i = next++; i < count; i = next++)
				fn(context, i);
		};
		std::thread other(work);
		work();
		other.join();
	}

private:
	ServerView *view;
};

// As the Framework's ReplicationManager: Receive() updates every plugin for each packet it hands
// over, so that Update() does nothing, and the owner runs the pass itself, from its own thread.
class ServerManager : public ReplicaManager3
{
public:
	explicit ServerManager(ServerView *view)
	    : view(view)
	{
	}

	void Update() override {}

	// Set before the threads start. With a world of its own, the pass thread also creates, changes and
	// destroys the server's replicas, destroys the clients' doomed ones, and walks the connections
	// after each pass.
	bool ownsAWorld = false;

	void RunPass()
	{
		if (!ownsAWorld)
		{
			ReplicaManager3::Update();
			++view->passes;
			return;
		}

		const TimeMS now = GetTimeMS();
		if (now - lastStep >= 5)
		{
			lastStep = now;
			// Away from the pass, whose locks would order this thread after the receive thread: what the
			// step changes must be safe on its own account.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Step();
		}

		// The world is held past the pass for what the owner then does with every connection it served:
		// the receive thread cannot drop one meanwhile.
		WorldGuard world(*this);
		ReplicaManager3::Update();
		unsigned int connections = 0;
		for (unsigned int i = 0; i < GetConnectionCount(); i++)
			connections += GetConnectionAtIndex(i)->GetRakNetGUID() != UNASSIGNED_RAKNET_GUID ? 1 : 0;
		view->connectionsSeenByPass += connections;
		++view->passes;
	}

	Connection_RM3 *AllocConnection(const SystemAddress &address, RakNetGUID guid) const override { return new ServerConnection(address, guid, view); }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }

protected:
	void OnNewConnection(const SystemAddress &address, RakNetGUID guid, bool isIncoming) override
	{
		ReplicaManager3::OnNewConnection(address, guid, isIncoming);
		++view->opened;
	}

	void OnClosedConnection(const SystemAddress &address, RakNetGUID guid, PI2_LostConnectionReason reason) override
	{
		ReplicaManager3::OnClosedConnection(address, guid, reason);
		++view->closed;
	}

private:
	void Step()
	{
		++steps;
		for (auto &replica : own)
			++replica->value;

		// One of the server's replicas comes or goes at every step.
		if (own.size() < SERVER_REPLICAS || steps % 2 == 0)
		{
			own.push_back(std::make_unique<ServerOwnedReplica>(steps % 4 != 0));
			Reference(own.back().get());
		}
		else
		{
			own.front()->BroadcastDestruction();
			Dereference(own.front().get());
			own.pop_front();
			++view->churned;
		}

		// And now and then one a client is sending state for goes, with that state still arriving.
		if (steps % 8 != 0)
			return;
		WorldGuard world(*this); // or the receive thread could drop its connection, and the replica with it
		MirroredReplica *replica = 0;
		{
			std::lock_guard<std::mutex> lock(view->doomedMutex);
			if (!view->doomed.empty())
				replica = view->doomed.back();
		}
		if (replica)
		{
			Dereference(replica);
			delete replica;
			++view->culled;
		}
	}

	ServerView *view;
	TimeMS lastStep = 0;
	unsigned int steps = 0;
	std::deque<std::unique_ptr<ServerOwnedReplica>> own;
};

// The server's two threads, joined before anything they use goes away.
class ServerThreads
{
public:
	ServerThreads(RakPeerInterface *peer, ServerManager *manager)
	    : receiveThread([this, peer]()
	                    {
		                    while (!stop.load())
		                    {
			                    peer->WaitForPackets(10);
			                    for (Packet *packet = peer->Receive(); packet; packet = peer->Receive())
				                    peer->DeallocatePacket(packet);
		                    } }),
	      passThread([this, manager]()
	                 {
		                 while (!stop.load())
		                 {
			                 manager->RunPass();
			                 std::this_thread::yield();
		                 } })
	{
	}

	~ServerThreads()
	{
		stop.store(true);
		receiveThread.join();
		passThread.join();
	}

private:
	std::atomic<bool> stop{false};
	std::thread receiveThread;
	std::thread passThread;
};

// A client, all of it on the test's thread.
struct Client
{
	NetworkIDManager ids;
	ClientStats stats;
	ClientManager manager{&stats};
	std::vector<std::unique_ptr<OwnedReplica>> replicas;
	Peer peer;
};

class Rm3ReceiveThread : public ::testing::Test
{
protected:
	void StartServer()
	{
		serverManager.SetNetworkIDManager(&serverIds);
		serverManager.SetAutoSerializeInterval(0);
		server->AttachPlugin(&serverManager);
		SocketDescriptor serverSocket(0, "127.0.0.1");
		ASSERT_EQ(RAKNET_STARTED, server->Startup(MAX_CLIENTS, &serverSocket, 1));
		server->SetMaximumIncomingConnections(MAX_CLIENTS);
		serverPort = server->GetInternalID().GetPort();
		serverAddress.SetBinaryAddress("127.0.0.1");
		serverAddress.SetPortHostOrder(serverPort);
	}

	// A started client holding its replicas, not yet connected. \a watchedReplicas of them are those whose
	// state the test waits for.
	void AddClient(unsigned int watchedReplicas)
	{
		RakSleep(2); // distinct microsecond-seeded GUIDs on POSIX
		const uint32_t first = static_cast<uint32_t>(clients.size()) * REPLICAS_PER_CLIENT;
		clients.push_back(std::make_unique<Client>());
		Client &client = *clients.back();
		client.manager.SetNetworkIDManager(&client.ids);
		client.manager.SetAutoSerializeInterval(0);
		client.peer->AttachPlugin(&client.manager);
		SocketDescriptor clientSocket;
		ASSERT_EQ(RAKNET_STARTED, client.peer->Startup(1, &clientSocket, 1));
		for (uint32_t i = 0; i < REPLICAS_PER_CLIENT; i++)
		{
			client.replicas.push_back(std::make_unique<OwnedReplica>());
			client.replicas.back()->index = first + i;
			client.manager.Reference(client.replicas.back().get());
			if (i < watchedReplicas)
				watched.push_back(first + i);
		}
	}

	// One turn of the clients' own single thread: every counter moves, and is sent.
	void PumpClients()
	{
		for (auto &client : clients)
		{
			for (auto &replica : client->replicas)
				++replica->counter;
			for (auto packet : client->peer.incoming())
				(void) packet;
			client->manager.RunPass();
		}
		RakSleep(5);
	}

	template <typename Condition>
	bool PumpUntil(Condition condition)
	{
		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 20000 && !condition())
			PumpClients();
		return condition();
	}

	// Pumps until every watched counter the server holds has moved past where it is now.
	bool PumpUntilEveryCounterAdvances()
	{
		std::vector<uint32_t> floor;
		for (unsigned int index : watched)
			floor.push_back(view.counters[index].load());
		return PumpUntil([&]()
		                 {
			                 for (size_t i = 0; i < watched.size(); i++)
			                 {
				                 if (view.counters[watched[i]].load() <= floor[i])
					                 return false;
			                 }
			                 return true; });
	}

	// Destroyed last to first: the threads stop, then the peers shut down, which empties the managers.
	ServerView view;
	NetworkIDManager serverIds;
	ServerManager serverManager{&view};
	TwoThreadExecutor executor{&view};
	std::vector<std::unique_ptr<Client>> clients;
	std::vector<unsigned int> watched;
	Peer server;
	unsigned short serverPort = 0;
	SystemAddress serverAddress;
	std::unique_ptr<ServerThreads> serverThreads;
};
} // namespace

// Nothing but Update() on one thread and Receive() on the other: the owner takes no lock of its own.
TEST_F(Rm3ReceiveThread, InboundStateArrivesWhileThePassRunsElsewhere)
{
	ASSERT_NO_FATAL_FAILURE(StartServer());
	ASSERT_NO_FATAL_FAILURE(AddClient(REPLICAS_PER_CLIENT));
	Client &client = *clients[0];

	serverThreads = std::make_unique<ServerThreads>(server.get(), &serverManager);
	const TimeMS start = GetTimeMS();

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client.peer->Connect("127.0.0.1", serverPort, 0, 0));
	ASSERT_TRUE(PumpUntilEveryCounterAdvances()) << "the server never received every replica's state";
	ASSERT_TRUE(PumpUntil([&]()
	                      { return GetTimeMS() - start >= 800; }));
	ASSERT_TRUE(PumpUntilEveryCounterAdvances()) << "state stopped arriving while the pass ran";

	// Closed and reopened mid-run: the server drops the connection and its replicas on the receive
	// thread, then takes them all again.
	client.peer->CloseConnection(serverAddress, true);
	ASSERT_TRUE(PumpUntil([&]()
	                      { return view.closed.load() == 1 && client.peer->GetConnectionState(serverAddress) == IS_DISCONNECTED; }));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client.peer->Connect("127.0.0.1", serverPort, 0, 0));
	ASSERT_TRUE(PumpUntil([&]()
	                      { return view.opened.load() == 2; }));
	ASSERT_TRUE(PumpUntilEveryCounterAdvances()) << "no state arrived over the reopened connection";

	ASSERT_TRUE(PumpUntil([&]()
	                      { return GetTimeMS() - start >= 2000; }));
	ASSERT_TRUE(PumpUntilEveryCounterAdvances());

	serverThreads.reset();
	EXPECT_EQ(2u, view.opened.load());
	EXPECT_EQ(1u, view.closed.load());
	EXPECT_GT(view.passes.load(), 10u);
	EXPECT_EQ(1u, serverManager.GetConnectionCount());
	EXPECT_EQ(REPLICAS_PER_CLIENT, serverManager.GetReplicaCount());
}

// The whole of it at once: two connections served across an executor, each sending state the whole
// time, while the pass thread serializes replicas of its own to both, creates and destroys them, and
// destroys replicas the clients are still sending state for.
TEST_F(Rm3ReceiveThread, ConnectionsAreServedInParallelWhileReplicasComeAndGo)
{
	view.cull = true;
	serverManager.ownsAWorld = true;
	serverManager.SetSerializeExecutor(&executor);
	ASSERT_NO_FATAL_FAILURE(StartServer());
	ASSERT_NO_FATAL_FAILURE(AddClient(KEPT_PER_CLIENT));
	ASSERT_NO_FATAL_FAILURE(AddClient(KEPT_PER_CLIENT));

	serverThreads = std::make_unique<ServerThreads>(server.get(), &serverManager);
	const TimeMS start = GetTimeMS();

	for (auto &client : clients)
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client->peer->Connect("127.0.0.1", serverPort, 0, 0));
	ASSERT_TRUE(PumpUntilEveryCounterAdvances()) << "the server never received every replica's state";

	// Every replica the pass thread was to destroy under its incoming state is gone, and the others
	// still get theirs.
	const unsigned int doomed = MAX_CLIENTS * (REPLICAS_PER_CLIENT - KEPT_PER_CLIENT);
	ASSERT_TRUE(PumpUntil([&]()
	                      { return view.culled.load() == doomed && GetTimeMS() - start >= 2000; }))
	    << view.culled.load() << " of " << doomed << " destroyed";
	ASSERT_TRUE(PumpUntilEveryCounterAdvances()) << "state stopped arriving";

	// Both clients were sent the server's replicas as they came, changed and went.
	const unsigned int updatesBefore[MAX_CLIENTS] = {clients[0]->stats.updates, clients[1]->stats.updates};
	ASSERT_TRUE(PumpUntil([&]()
	                      { return clients[0]->stats.updates > updatesBefore[0] + 100 && clients[1]->stats.updates > updatesBefore[1] + 100; }));
	for (auto &client : clients)
	{
		EXPECT_GT(client->stats.constructed, SERVER_REPLICAS);
		EXPECT_GT(client->stats.destroyed, 0u);
	}

	serverThreads.reset();
	EXPECT_EQ(MAX_CLIENTS, view.opened.load());
	EXPECT_EQ(0u, view.closed.load());
	EXPECT_GT(view.executorRuns.load(), 10u);
	EXPECT_GT(view.churned.load(), 10u);
	EXPECT_GT(view.connectionsSeenByPass.load(), 0u);
	EXPECT_EQ(MAX_CLIENTS, serverManager.GetConnectionCount());
}
