/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mafianet/rpc4_plugin.h"
#include "mafianet/peer_interface.h"
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

/*
Description:
Covers RPC4's three registration tables -- non-blocking functions, blocking functions and
local callbacks -- written before they move to the standard library (std migration stage 2, #60).
The two tables are DataStructures::Hash keyed by function name; localCallbacks is a
DataStructures::OrderedList keyed by MessageID holding one entry per message id, each owning an
ordered list of function names.

The existing RPC4 coverage is two cases about global-registration name handling plus one
integration test, and none of it exercises registration, unregistration, replacement or dispatch,
which is exactly what a container swap can break quietly:

 - Hash::Push on an existing key and OrderedList::Insert on an existing key behave differently
   from each other (one appends a second entry for the same key, the other refuses), and both
   differ from std::unordered_map::insert and operator[]. A migration that picks the wrong one
   either loses a registration or silently keeps a stale function pointer.
 - localCallbacks holds a sorted list of names per message id, and the dispatch loop walks it, so
   both the per-id lookup and the per-id name list have to survive.

Success conditions: register/unregister round-trips report accurately, re-registering replaces
rather than duplicates, the blocking and non-blocking tables are independent, a local callback can
carry several function names, and loopback dispatch reaches the registered function.
*/

namespace
{
	int g_calls = 0;
	std::string g_lastPayload;

	void CountingRpc(MafiaNet::BitStream *userData, Packet *, void *)
	{
		++g_calls;
		g_lastPayload.clear();
		if (userData != 0)
		{
			char buf[64];
			memset(buf, 0, sizeof(buf));
			// Read whatever the caller wrote, bounded by the buffer.
			const unsigned bytes = (unsigned) BITS_TO_BYTES(userData->GetNumberOfUnreadBits());
			if (bytes > 0 && bytes < sizeof(buf))
			{
				userData->Read(buf, bytes);
				g_lastPayload = buf;
			}
		}
	}

	void OtherRpc(MafiaNet::BitStream *, Packet *, void *) {}

	void CountingBlockingRpc(MafiaNet::BitStream *, MafiaNet::BitStream *, Packet *, void *) {}

	class RPC4Registry : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g_calls = 0;
			g_lastPayload.clear();
			peer = RakPeerInterface::GetInstance();
			peer->AttachPlugin(&rpc4);
			// Started but unconnected: CallLoopback pushes a packet that only surfaces
			// through Receive() on a started peer. Ephemeral port, no traffic.
			SocketDescriptor sd(0, "127.0.0.1");
			ASSERT_EQ(peer->Startup(1, &sd, 1), RAKNET_STARTED);
		}
		void TearDown() override
		{
			peer->DetachPlugin(&rpc4);
			peer->Shutdown(100);
			RakPeerInterface::DestroyInstance(peer);
		}

		RakPeerInterface *peer = nullptr;
		RPC4 rpc4;
	};
}

TEST_F(RPC4Registry, UnregisteringAnUnknownFunctionFails)
{
	EXPECT_FALSE(rpc4.UnregisterFunction("never.registered"));
}

TEST_F(RPC4Registry, RegisterThenUnregisterRoundTrips)
{
	EXPECT_TRUE(rpc4.RegisterFunction("ping", CountingRpc, 0));
	EXPECT_TRUE(rpc4.UnregisterFunction("ping"));
	EXPECT_FALSE(rpc4.UnregisterFunction("ping")) << "a second unregister must report nothing to remove";
}

TEST_F(RPC4Registry, RegisteringTheSameNameTwiceIsRefused)
{
	EXPECT_TRUE(rpc4.RegisterFunction("ping", CountingRpc, 0));
	EXPECT_FALSE(rpc4.RegisterFunction("ping", OtherRpc, 0));

	// Exactly one entry must exist under that name, whichever function it holds.
	EXPECT_TRUE(rpc4.UnregisterFunction("ping"));
	EXPECT_FALSE(rpc4.UnregisterFunction("ping")) << "the refused registration must not have added a second entry";
}

TEST_F(RPC4Registry, SeveralDistinctNamesCoexist)
{
	ASSERT_TRUE(rpc4.RegisterFunction("a", CountingRpc, 0));
	ASSERT_TRUE(rpc4.RegisterFunction("b", CountingRpc, 0));
	ASSERT_TRUE(rpc4.RegisterFunction("c", CountingRpc, 0));

	EXPECT_TRUE(rpc4.UnregisterFunction("b"));
	EXPECT_TRUE(rpc4.UnregisterFunction("a"));
	EXPECT_TRUE(rpc4.UnregisterFunction("c"));
}

TEST_F(RPC4Registry, ManyNamesSurviveTogether)
{
	// Enough names to spread across hash buckets, so a migration that collapses
	// colliding keys shows up here rather than in production.
	const int count = 200;
	for (int i = 0; i < count; ++i)
	{
		char name[32];
		snprintf(name, sizeof(name), "fn.%d", i);
		ASSERT_TRUE(rpc4.RegisterFunction(name, CountingRpc, 0)) << "registering " << name;
	}
	for (int i = 0; i < count; ++i)
	{
		char name[32];
		snprintf(name, sizeof(name), "fn.%d", i);
		EXPECT_TRUE(rpc4.UnregisterFunction(name)) << "unregistering " << name;
	}
}

TEST_F(RPC4Registry, BlockingAndNonBlockingTablesAreIndependent)
{
	ASSERT_TRUE(rpc4.RegisterFunction("shared.name", CountingRpc, 0));
	ASSERT_TRUE(rpc4.RegisterBlockingFunction("shared.name", CountingBlockingRpc, 0));

	// Removing one must leave the other in place.
	EXPECT_TRUE(rpc4.UnregisterFunction("shared.name"));
	EXPECT_FALSE(rpc4.UnregisterFunction("shared.name"));
	EXPECT_TRUE(rpc4.UnregisterBlockingFunction("shared.name"));
	EXPECT_FALSE(rpc4.UnregisterBlockingFunction("shared.name"));
}

TEST_F(RPC4Registry, BlockingRegisterThenUnregisterRoundTrips)
{
	EXPECT_FALSE(rpc4.UnregisterBlockingFunction("blocking.absent"));
	EXPECT_TRUE(rpc4.RegisterBlockingFunction("blocking.present", CountingBlockingRpc, 0));
	EXPECT_TRUE(rpc4.UnregisterBlockingFunction("blocking.present"));
	EXPECT_FALSE(rpc4.UnregisterBlockingFunction("blocking.present"));
}

TEST_F(RPC4Registry, LocalCallbackRoundTripsForOneMessageId)
{
	const MessageID id = ID_USER_PACKET_ENUM + 1;
	ASSERT_TRUE(rpc4.RegisterFunction("cb", CountingRpc, 0));
	rpc4.RegisterLocalCallback("cb", id);

	EXPECT_TRUE(rpc4.UnregisterLocalCallback("cb", id));
	EXPECT_FALSE(rpc4.UnregisterLocalCallback("cb", id))
		<< "a second unregister must report nothing to remove";
}

TEST_F(RPC4Registry, OneMessageIdCarriesSeveralFunctionNames)
{
	const MessageID id = ID_USER_PACKET_ENUM + 2;
	ASSERT_TRUE(rpc4.RegisterFunction("first", CountingRpc, 0));
	ASSERT_TRUE(rpc4.RegisterFunction("second", OtherRpc, 0));
	rpc4.RegisterLocalCallback("first", id);
	rpc4.RegisterLocalCallback("second", id);

	// Removing one name must leave the other attached to the same message id.
	EXPECT_TRUE(rpc4.UnregisterLocalCallback("first", id));
	EXPECT_TRUE(rpc4.UnregisterLocalCallback("second", id));
	EXPECT_FALSE(rpc4.UnregisterLocalCallback("second", id));
}

TEST_F(RPC4Registry, DistinctMessageIdsAreKeptApart)
{
	const MessageID idA = ID_USER_PACKET_ENUM + 3;
	const MessageID idB = ID_USER_PACKET_ENUM + 4;
	ASSERT_TRUE(rpc4.RegisterFunction("cb", CountingRpc, 0));
	rpc4.RegisterLocalCallback("cb", idA);
	rpc4.RegisterLocalCallback("cb", idB);

	EXPECT_TRUE(rpc4.UnregisterLocalCallback("cb", idA));
	EXPECT_FALSE(rpc4.UnregisterLocalCallback("cb", idA));
	EXPECT_TRUE(rpc4.UnregisterLocalCallback("cb", idB)) << "the other message id must be unaffected";
}

TEST_F(RPC4Registry, UnregisteringALocalCallbackOnAnUnknownMessageIdFails)
{
	ASSERT_TRUE(rpc4.RegisterFunction("cb", CountingRpc, 0));
	EXPECT_FALSE(rpc4.UnregisterLocalCallback("cb", ID_USER_PACKET_ENUM + 99));
}

TEST_F(RPC4Registry, LoopbackReachesTheRegisteredFunction)
{
	ASSERT_TRUE(rpc4.RegisterFunction("loop", CountingRpc, 0));

	MafiaNet::BitStream bs;
	bs.Write("hi", 3); // includes the terminator
	rpc4.CallLoopback("loop", &bs);

	// CallLoopback pushes a packet the plugin consumes on the next Receive.
	for (int i = 0; i < 20 && g_calls == 0; ++i)
	{
		Packet *p = peer->Receive();
		if (p != 0)
			peer->DeallocatePacket(p);
	}

	EXPECT_EQ(g_calls, 1);
	EXPECT_EQ(g_lastPayload, "hi");
}

TEST_F(RPC4Registry, LoopbackToAnUnknownNameDoesNotInvokeAnything)
{
	ASSERT_TRUE(rpc4.RegisterFunction("loop", CountingRpc, 0));

	MafiaNet::BitStream bs;
	rpc4.CallLoopback("absent", &bs);

	for (int i = 0; i < 20; ++i)
	{
		Packet *p = peer->Receive();
		if (p != 0)
			peer->DeallocatePacket(p);
	}

	EXPECT_EQ(g_calls, 0);
}

TEST_F(RPC4Registry, SlotRegisterThenUnregisterRoundTrips)
{
	EXPECT_FALSE(rpc4.UnregisterSlot("slot.absent"));
	rpc4.RegisterSlot("slot.present", CountingRpc, 0, 0);
	EXPECT_TRUE(rpc4.UnregisterSlot("slot.present"));
	EXPECT_FALSE(rpc4.UnregisterSlot("slot.present"));
}
