/*
 *  Copyright (c) 2024, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "RPC4ContextTest.h"

#include "mafianet/RPC4Plugin.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"

#include <atomic>
#include <thread>

/*
Description:
Tests that RPC4 handlers carry an opaque user-context pointer.

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
		(void)userData;
		(void)packet;
		if (g_observed.slotInvocations == 0)
			g_observed.firstSlotContext = context;
		else
			g_observed.secondSlotContext = context;
		g_observed.slotInvocations++;
	}

	void FunctionHandler(MafiaNet::BitStream *userData, Packet *packet, void *context)
	{
		(void)userData;
		(void)packet;
		g_observed.functionContext = context;
		g_observed.functionInvocations++;
	}

	void BlockingHandler(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context)
	{
		(void)userData;
		(void)packet;
		g_observed.blockingContext = context;
		g_observed.blockingInvocations++;
		returnData->Write((unsigned char)1); // CallBlocking requires a reply to unblock the caller
	}
}

int RPC4ContextTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void)params;
	(void)isVerbose;
	(void)noPauses;

	ResetObservation();
	destroyList.Clear(false, _FILE_AND_LINE_);

	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	destroyList.Push(peer, _FILE_AND_LINE_);

	SocketDescriptor sd(0, 0);
	if (peer->Startup(8, &sd, 1) != RAKNET_STARTED)
		return 1;

	RPC4 rpc;
	peer->AttachPlugin(&rpc);

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
	rpc.Signal("Event", nullptr, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true, true);

	if (g_observed.slotInvocations != 2)
		return 2;
	if (g_observed.firstSlotContext != &markerA)
		return 3;
	if (g_observed.secondSlotContext != &markerB)
		return 4;

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

	if (g_observed.functionInvocations == 0)
		return 5;
	if (g_observed.functionContext != &markerFn)
		return 6;

	// --- Blocking function context (registeredBlockingFunctions map) ---
	// Needs a real remote: CallBlocking sends ID_RPC4_CALL with isBlocking=true,
	// the server's OnReceive invokes the handler with its context, then replies.
	RakPeerInterface *server = RakPeerInterface::GetInstance();
	destroyList.Push(server, _FILE_AND_LINE_);

	SocketDescriptor serverSd(0, 0);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
		return 7;
	server->SetMaximumIncomingConnections(8);

	RPC4 serverRpc;
	server->AttachPlugin(&serverRpc);
	int markerBlocking = 0;
	serverRpc.RegisterBlockingFunction("Blocking", BlockingHandler, &markerBlocking);

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
	if (!connected)
		return 8;

	// CallBlocking blocks the caller (pumping only the client's Receive), so run
	// it on a worker thread while the main thread drives the server.
	MafiaNet::BitStream blockingParams;
	blockingParams.Write((unsigned char)42);
	RakNetGUID serverGuid = server->GetMyGUID();
	std::atomic<int> blockingResult(-1); // -1 = running, 0 = false, 1 = true
	std::thread caller([&]() {
		MafiaNet::BitStream returnData;
		bool ok = rpc.CallBlocking("Blocking", &blockingParams, HIGH_PRIORITY, RELIABLE_ORDERED, 0, serverGuid, &returnData);
		blockingResult.store(ok ? 1 : 0);
	});

	for (int i = 0; i < 500 && blockingResult.load() == -1; i++)
	{
		Packet *p;
		for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
			;
		RakSleep(10);
	}
	caller.join();

	if (blockingResult.load() != 1 || g_observed.blockingInvocations == 0)
		return 9;
	if (g_observed.blockingContext != &markerBlocking)
		return 10;

	return 0;
}

RakString RPC4ContextTest::GetTestName()
{
	return "RPC4ContextTest";
}

RakString RPC4ContextTest::ErrorCodeToString(int errorCode)
{
	if (errorCode > 0 && (unsigned int)errorCode <= errorList.Size())
		return errorList[errorCode - 1];
	return "Undefined Error";
}

RPC4ContextTest::RPC4ContextTest(void)
{
	errorList.Push("Peer failed to start", _FILE_AND_LINE_);
	errorList.Push("Slot handler was not invoked exactly twice", _FILE_AND_LINE_);
	errorList.Push("First slot did not receive its registered context", _FILE_AND_LINE_);
	errorList.Push("Second slot did not receive its registered context", _FILE_AND_LINE_);
	errorList.Push("Nonblocking function handler was not invoked", _FILE_AND_LINE_);
	errorList.Push("Nonblocking function did not receive its registered context", _FILE_AND_LINE_);
	errorList.Push("Server peer failed to start", _FILE_AND_LINE_);
	errorList.Push("Client failed to connect to server", _FILE_AND_LINE_);
	errorList.Push("Blocking function handler was not invoked (or call failed)", _FILE_AND_LINE_);
	errorList.Push("Blocking function did not receive its registered context", _FILE_AND_LINE_);
}

RPC4ContextTest::~RPC4ContextTest(void)
{
}

void RPC4ContextTest::DestroyPeers()
{
	int theSize = destroyList.Size();
	for (int i = 0; i < theSize; i++)
		destroyList[i]->Shutdown(100);
	for (int i = 0; i < theSize; i++)
		RakPeerInterface::DestroyInstance(destroyList[i]);
}
