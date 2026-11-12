/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// A server that receives on one thread while another runs ReplicaManager3's outbound pass: the
// receive thread loops WaitForPackets()/Receive(), which is where a connection opens, closes and
// delivers its replicas and their state, and the pass thread serializes them back.

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
#include <memory>
#include <thread>
#include <vector>

using namespace MafiaNet;

namespace
{
const unsigned int REPLICA_COUNT = 64;

// What the test thread reads of the server, which only its two threads may touch.
struct ServerView
{
	std::atomic<uint32_t> counters[REPLICA_COUNT] = {};
	std::atomic<unsigned int> opened{0};
	std::atomic<unsigned int> closed{0};
	std::atomic<unsigned int> passes{0};
	std::atomic<unsigned int> connectionsSeenByPass{0};
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

// The server's copy. Its counter is a plain field on purpose: the receive thread writes it in
// Deserialize() and the pass thread reads it in Serialize() for the same connection, which
// ReplicaManager3 keeps apart.
class MirroredReplica : public Replica3
{
public:
	explicit MirroredReplica(ServerView *view)
	    : view(view)
	{
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
		if (!parameters->serializationBitstream[0].Read(index) || !parameters->serializationBitstream[0].Read(counter) || index >= REPLICA_COUNT)
			return;
		view->counters[index].store(counter);
	}

	RM3SerializationResult Serialize(SerializeParameters *) override
	{
		lastSerialized = counter;
		return RM3SR_DO_NOT_SERIALIZE;
	}

	ServerView *view;
	uint32_t counter = 0;
	uint32_t lastSerialized = 0;
};

class ClientConnection : public Connection_RM3
{
public:
	using Connection_RM3::Connection_RM3;
	Replica3 *AllocReplica(BitStream *, ReplicaManager3 *) override { return 0; }
};

class ClientManager : public ReplicaManager3
{
public:
	Connection_RM3 *AllocConnection(const SystemAddress &address, RakNetGUID guid) const override { return new ClientConnection(address, guid); }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }
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

	void RunPass()
	{
		// The world is held past the pass for what the owner then does with every connection it served:
		// the receive thread cannot drop one meanwhile.
		LockWorld();
		ReplicaManager3::Update();
		unsigned int connections = 0;
		for (unsigned int i = 0; i < GetConnectionCount(); i++)
			connections += GetConnectionAtIndex(i)->GetRakNetGUID() != UNASSIGNED_RAKNET_GUID ? 1 : 0;
		UnlockWorld();
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
	ServerView *view;
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
			                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

class Rm3ReceiveThread : public ::testing::Test
{
protected:
	// One turn of the client's own single thread: every counter moves, and Receive() runs its pass.
	void PumpClient()
	{
		for (auto &replica : replicas)
			++replica->counter;
		for (auto packet : client.incoming())
			(void) packet;
		RakSleep(5);
	}

	// Pumps until every counter the server holds is past \a floor, or 10 seconds went by.
	bool PumpUntilEveryCounterPassed(const std::vector<uint32_t> &floor)
	{
		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 10000)
		{
			PumpClient();
			bool all = true;
			for (unsigned int i = 0; i < REPLICA_COUNT && all; i++)
				all = view.counters[i].load() > floor[i];
			if (all)
				return true;
		}
		return false;
	}

	template <typename Condition>
	bool PumpUntil(Condition condition)
	{
		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 10000 && !condition())
			PumpClient();
		return condition();
	}

	std::vector<uint32_t> ServerCounters() const
	{
		std::vector<uint32_t> counters(REPLICA_COUNT);
		for (unsigned int i = 0; i < REPLICA_COUNT; i++)
			counters[i] = view.counters[i].load();
		return counters;
	}

	// Destroyed last to first: the threads stop, then the peers shut down, which empties both managers.
	ServerView view;
	NetworkIDManager serverIds;
	NetworkIDManager clientIds;
	ServerManager serverManager{&view};
	ClientManager clientManager;
	std::vector<std::unique_ptr<OwnedReplica>> replicas;
	Peer server;
	Peer client;
	std::unique_ptr<ServerThreads> serverThreads;
};
} // namespace

TEST_F(Rm3ReceiveThread, InboundStateArrivesWhileThePassRunsElsewhere)
{
	serverManager.SetNetworkIDManager(&serverIds);
	serverManager.SetAutoSerializeInterval(0);
	server->AttachPlugin(&serverManager);
	SocketDescriptor serverSocket(0, "127.0.0.1");
	ASSERT_EQ(RAKNET_STARTED, server->Startup(1, &serverSocket, 1));
	server->SetMaximumIncomingConnections(1);
	const unsigned short serverPort = server->GetInternalID().GetPort();
	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);

	RakSleep(2); // distinct microsecond-seeded GUIDs on POSIX
	clientManager.SetNetworkIDManager(&clientIds);
	clientManager.SetAutoSerializeInterval(0);
	client->AttachPlugin(&clientManager);
	SocketDescriptor clientSocket;
	ASSERT_EQ(RAKNET_STARTED, client->Startup(1, &clientSocket, 1));
	for (uint32_t i = 0; i < REPLICA_COUNT; i++)
	{
		replicas.push_back(std::make_unique<OwnedReplica>());
		replicas.back()->index = i;
		clientManager.Reference(replicas.back().get());
	}

	serverThreads = std::make_unique<ServerThreads>(server.get(), &serverManager);
	const TimeMS start = GetTimeMS();

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client->Connect("127.0.0.1", serverPort, 0, 0));
	ASSERT_TRUE(PumpUntilEveryCounterPassed(std::vector<uint32_t>(REPLICA_COUNT, 0))) << "the server never received every replica's state";
	ASSERT_TRUE(PumpUntil([&]()
	                      { return GetTimeMS() - start >= 800; }));
	ASSERT_TRUE(PumpUntilEveryCounterPassed(ServerCounters())) << "state stopped arriving while the pass ran";

	// Closed and reopened mid-run: the server drops the connection and its replicas on the receive
	// thread, then takes them all again.
	client->CloseConnection(serverAddress, true);
	ASSERT_TRUE(PumpUntil([&]()
	                      { return view.closed.load() == 1 && client->GetConnectionState(serverAddress) == IS_DISCONNECTED; }));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client->Connect("127.0.0.1", serverPort, 0, 0));
	ASSERT_TRUE(PumpUntil([&]()
	                      { return view.opened.load() == 2; }));
	ASSERT_TRUE(PumpUntilEveryCounterPassed(ServerCounters())) << "no state arrived over the reopened connection";

	ASSERT_TRUE(PumpUntil([&]()
	                      { return GetTimeMS() - start >= 2000; }));
	ASSERT_TRUE(PumpUntilEveryCounterPassed(ServerCounters()));

	serverThreads.reset();
	EXPECT_EQ(2u, view.opened.load());
	EXPECT_EQ(1u, view.closed.load());
	EXPECT_GT(view.passes.load(), 10u);
	EXPECT_GT(view.connectionsSeenByPass.load(), 0u);
	EXPECT_EQ(1u, serverManager.GetConnectionCount());
	EXPECT_EQ(REPLICA_COUNT, serverManager.GetReplicaCount());
}
