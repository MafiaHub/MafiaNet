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

Failure conditions: any of the above does not hold.
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
	};
	Observation g_observed;

	void ResetObservation()
	{
		g_observed.firstSlotContext = nullptr;
		g_observed.secondSlotContext = nullptr;
		g_observed.slotInvocations = 0;
		g_observed.functionContext = nullptr;
		g_observed.functionInvocations = 0;
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
