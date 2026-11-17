/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include <vector>
#include "mafianet/peer_interface.h"
#include "mafianet/rpc.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/sleep.h"

#include <atomic>
#include <thread>

using namespace MafiaNet;

/*
Description:
Tests that RPC handlers carry an opaque user-context pointer.

Each RegisterSlot / RegisterFunction / RegisterBlockingFunction call stores a
void* context alongside the function pointer; when the handler runs it is
invoked with that exact context. This lets a handler recover the object
instance it belongs to without a file-static global pointer, and lets two
registrations of the SAME handler under the SAME identifier carry distinct
contexts (e.g. two object instances on one peer).

Success conditions:
- A slot handler receives the context it was registered with.
- Two slots sharing an identifier each receive their own distinct context.
- A nonblocking function handler receives its registered context.
- A blocking function handler receives its registered context.

Failure conditions: any of the above does not hold.

Note: the blocking case needs a second (server) peer. CallBlocking pumps only
the caller's Receive() while it waits, so the server is driven from another
thread; each RakPeerInterface is still only ever touched by one thread.
*/

namespace
{
// Observation state for the C-style handlers. The feature under test is what
// lets production code AVOID globals; the test itself may use file-static
// state to observe what each invocation received.
struct Observation
{
	void *firstSlotContext;
	void *secondSlotContext;
	int slotInvocations;
	void *functionContext;
	int functionInvocations;
	void *blockingContext;
	int blockingInvocations;
};
Observation g_observed;

void ResetObservation()
{
	g_observed.firstSlotContext = nullptr;
	g_observed.secondSlotContext = nullptr;
	g_observed.slotInvocations = 0;
	g_observed.functionContext = nullptr;
	g_observed.functionInvocations = 0;
	g_observed.blockingContext = nullptr;
	g_observed.blockingInvocations = 0;
}

void SlotHandler(MafiaNet::BitStream *userData, Packet *packet, void *context)
{
	(void) userData;
	(void) packet;
	if (g_observed.slotInvocations == 0)
		g_observed.firstSlotContext = context;
	else
		g_observed.secondSlotContext = context;
	g_observed.slotInvocations++;
}

void FunctionHandler(MafiaNet::BitStream *userData, Packet *packet, void *context)
{
	(void) userData;
	(void) packet;
	g_observed.functionContext = context;
	g_observed.functionInvocations++;
}

void BlockingHandler(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context)
{
	(void) userData;
	(void) packet;
	g_observed.blockingContext = context;
	g_observed.blockingInvocations++;
	returnData->Write((unsigned char) 1); // CallBlocking requires a reply to unblock the caller
}
}

class RPCContext : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ResetObservation();
	}

	void TearDown() override
	{
		const std::size_t theSize = destroyList.size();
		for (std::size_t i = 0; i < theSize; i++)
			destroyList[i]->Shutdown(100);
		for (std::size_t i = 0; i < theSize; i++)
			RakPeerInterface::DestroyInstance(destroyList[i]);
	}

	std::vector<RakPeerInterface *> destroyList;
};

TEST_F(RPCContext, HandlersReceiveRegisteredContext)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	destroyList.push_back(peer);

	SocketDescriptor sd(0, 0);
	ASSERT_EQ(peer->Startup(8, &sd, 1), RAKNET_STARTED) << "Peer failed to start";

	RPC &rpc = peer->GetRPC();

	// Two distinct context markers. Their addresses are what we assert on.
	int markerA = 0;
	int markerB = 0;
	int markerFn = 0;

	// Register the SAME handler twice under the same identifier with different
	// contexts. This is the core scenario: two object instances on one peer.
	rpc.RegisterSlot("Event", SlotHandler, &markerA, 0);
	rpc.RegisterSlot("Event", SlotHandler, &markerB, 0);

	// Broadcasting to zero connections is a no-op; invokeLocal=true drives the
	// local handlers synchronously through InvokeSignal.
	rpc.Signal("Event", nullptr, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true, true);

	ASSERT_EQ(g_observed.slotInvocations, 2) << "Slot handler was not invoked exactly twice";
	ASSERT_EQ(g_observed.firstSlotContext, &markerA) << "First slot did not receive its registered context";
	ASSERT_EQ(g_observed.secondSlotContext, &markerB) << "Second slot did not receive its registered context";

	// Nonblocking function context travels through the registeredNonblockingFunctions map.
	rpc.RegisterFunction("Fn", FunctionHandler, &markerFn);
	rpc.CallLoopback("Fn", nullptr);

	// CallLoopback queues a packet processed by the plugin during Receive().
	for (int i = 0; i < 100 && g_observed.functionInvocations == 0; i++)
	{
		Packet *p;
		for (p = peer->Receive(); p; peer->DeallocatePacket(p), p = peer->Receive())
			;
		RakSleep(10);
	}

	ASSERT_NE(g_observed.functionInvocations, 0) << "Nonblocking function handler was not invoked";
	ASSERT_EQ(g_observed.functionContext, &markerFn) << "Nonblocking function did not receive its registered context";

	// --- Blocking function context (registeredBlockingFunctions map) ---
	// Needs a real remote: CallBlocking sends ID_RPC_CALL with isBlocking=true,
	// the server's OnReceive invokes the handler with its context, then replies.
	RakPeerInterface *server = RakPeerInterface::GetInstance();
	destroyList.push_back(server);

	SocketDescriptor serverSd(0, 0);
	ASSERT_EQ(server->Startup(8, &serverSd, 1), RAKNET_STARTED) << "Server peer failed to start";
	server->SetMaximumIncomingConnections(8);

	int markerBlocking = 0;
	server->GetRPC().RegisterBlockingFunction("Blocking", BlockingHandler, &markerBlocking);

	unsigned short serverPort = server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
	peer->Connect("127.0.0.1", serverPort, nullptr, 0);

	// Pump both peers until the client reports the connection accepted.
	bool connected = false;
	for (int i = 0; i < 500 && !connected; i++)
	{
		Packet *p;
		for (p = peer->Receive(); p; peer->DeallocatePacket(p), p = peer->Receive())
		{
			if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				connected = true;
		}
		for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
			;
		RakSleep(10);
	}
	ASSERT_TRUE(connected) << "Client failed to connect to server";

	// CallBlocking blocks the caller (pumping only the client's Receive), so run
	// it on a worker thread while the main thread drives the server.
	MafiaNet::BitStream blockingParams;
	blockingParams.Write((unsigned char) 42);
	RakNetGUID serverGuid = server->GetMyGUID();
	std::atomic<int> blockingResult(-1); // -1 = running, 0 = false, 1 = true
	std::thread caller([&]()
	                   {
		MafiaNet::BitStream returnData;
		const unsigned char userMessage = ID_USER_PACKET_ENUM;
		peer->SendLoopback(reinterpret_cast<const char *>(&userMessage), 1);
		bool ok = rpc.CallBlocking("Blocking", &blockingParams, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, serverGuid, &returnData);
		unsigned char value = 0;
		const bool replied = ok && returnData.Read(value) && value == 1;
		const bool missing = rpc.CallBlocking("Missing", nullptr, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, serverGuid, &returnData);
		bool receivedUserMessage = false;
		for (Packet *p = peer->Receive(); p; peer->DeallocatePacket(p), p = peer->Receive())
			receivedUserMessage |= p->data[0] == ID_USER_PACKET_ENUM;
		blockingResult.store(replied && !missing && receivedUserMessage ? 1 : 0); });

	for (int i = 0; i < 500 && blockingResult.load() == -1; i++)
	{
		Packet *p;
		for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
			;
		RakSleep(10);
	}
	if (blockingResult.load() == -1)
		server->Shutdown(100);
	caller.join();

	ASSERT_TRUE(blockingResult.load() == 1 && g_observed.blockingInvocations != 0)
	    << "Blocking function handler was not invoked (or call failed)";
	ASSERT_EQ(g_observed.blockingContext, &markerBlocking)
	    << "Blocking function did not receive its registered context";
}
