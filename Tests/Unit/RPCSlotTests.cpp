/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RPC slot handles (GetLocalSlot / InvokeSignal) and the std:: tables behind
 *  RPC's functions, slots and local callbacks. Peer-level cases run on the
 *  simulated network.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer.h"
#include "mafianet/remote_procedure_call.h"

#include <string>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_copy_constructible_v<RPC>, "RPC owns its slots");
static_assert(!std::is_copy_assignable_v<RPC>, "RPC owns its slots");

namespace
{
// Each slot appends its tag and the first value it reads from the parameters.
struct CallLog
{
	std::vector<std::string> calls;
	bool interruptAfterFirst = false;
	RPC *rpc = nullptr;
};

struct SlotContext
{
	CallLog *log;
	const char *tag;
};

void RecordingSlot(BitStream *userData, Packet *, void *context)
{
	SlotContext *ctx = static_cast<SlotContext *>(context);
	int value = -1;
	userData->Read(value);
	ctx->log->calls.push_back(std::string(ctx->tag) + ":" + std::to_string(value));
	if (ctx->log->interruptAfterFirst)
		ctx->log->rpc->InterruptSignal();
}

void CountingFunction(BitStream *, Packet *, void *context)
{
	++*static_cast<int *>(context);
}
} // namespace

TEST(RPCSlot, ReceiveConsumesMalformedRpcFramesAndDeliversFollowingPacket)
{
	SimulatedNetwork net;
	auto *peer = net.AddPeer(1);
	ASSERT_NE(nullptr, peer);
	const unsigned char frames[][2] = {{ID_RPC, 0}, {ID_RPC, 0xee}, {ID_USER_PACKET_ENUM, 0}};
	peer->SendLoopback(reinterpret_cast<const char *>(frames[0]), 1);
	peer->SendLoopback(reinterpret_cast<const char *>(frames[1]), 2);
	peer->SendLoopback(reinterpret_cast<const char *>(frames[2]), 2);
	Packet *packet = peer->Receive();
	ASSERT_NE(nullptr, packet);
	EXPECT_EQ(ID_USER_PACKET_ENUM, packet->data[0]);
	peer->DeallocatePacket(packet);
	EXPECT_EQ(nullptr, peer->Receive());
}

TEST(RPCSlot, GetLocalSlotReturnsNullForUnknownName)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	EXPECT_EQ(nullptr, rpc.GetLocalSlot("missing"));

	CallLog log;
	SlotContext ctx{&log, "a"};
	rpc.RegisterSlot("present", RecordingSlot, &ctx, 0);
	EXPECT_NE(nullptr, rpc.GetLocalSlot("present"));
	EXPECT_EQ(nullptr, rpc.GetLocalSlot("missing"));
	EXPECT_EQ(nullptr, rpc.GetLocalSlot("presen"));
}

TEST(RPCSlot, InvokeSignalByHandleCallsRegisteredSlots)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	CallLog log;
	SlotContext low{&log, "low"}, firstHigh{&log, "high1"}, secondHigh{&log, "high2"};
	rpc.RegisterSlot("event", RecordingSlot, &low, 1);
	rpc.RegisterSlot("event", RecordingSlot, &firstHigh, 5);
	rpc.RegisterSlot("event", RecordingSlot, &secondHigh, 5);

	RPC::LocalSlot *slot = rpc.GetLocalSlot("event");
	ASSERT_NE(nullptr, slot);

	BitStream parameters;
	parameters.Write(42);
	Packet packet;
	rpc.InvokeSignal(slot, &parameters, &packet);

	// Highest priority first; equal priorities in registration order; every slot reads from the start.
	const std::vector<std::string> expected = {"high1:42", "high2:42", "low:42"};
	EXPECT_EQ(expected, log.calls);
}

TEST(RPCSlot, InvokeSignalWithNullHandleDoesNothing)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	BitStream parameters;
	Packet packet;
	rpc.InvokeSignal(nullptr, &parameters, &packet);
	SUCCEED();
}

TEST(RPCSlot, HandleStaysValidAcrossRegistrationsAndRehash)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	CallLog log;
	SlotContext first{&log, "first"}, later{&log, "later"};
	rpc.RegisterSlot("stable", RecordingSlot, &first, 0);
	RPC::LocalSlot *slot = rpc.GetLocalSlot("stable");
	ASSERT_NE(nullptr, slot);

	// Enough other identifiers to force the table to rehash several times.
	for (int i = 0; i < 2000; i++)
		rpc.RegisterSlot(("other" + std::to_string(i)).c_str(), RecordingSlot, &first, 0);
	rpc.RegisterSlot("stable", RecordingSlot, &later, 0);

	EXPECT_EQ(slot, rpc.GetLocalSlot("stable"));
	BitStream parameters;
	parameters.Write(7);
	Packet packet;
	rpc.InvokeSignal(slot, &parameters, &packet);
	const std::vector<std::string> expected = {"first:7", "later:7"};
	EXPECT_EQ(expected, log.calls);
}

TEST(RPCSlot, UnregisterSlotRemovesTheHandle)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	CallLog log;
	SlotContext ctx{&log, "a"};
	rpc.RegisterSlot("gone", RecordingSlot, &ctx, 0);
	EXPECT_TRUE(rpc.UnregisterSlot("gone"));
	EXPECT_FALSE(rpc.UnregisterSlot("gone"));
	EXPECT_EQ(nullptr, rpc.GetLocalSlot("gone"));
}

TEST(RPCSlot, InterruptSignalStopsTheRemainingSlots)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	CallLog log;
	log.rpc = &rpc;
	log.interruptAfterFirst = true;
	SlotContext a{&log, "a"}, b{&log, "b"};
	rpc.RegisterSlot("event", RecordingSlot, &a, 2);
	rpc.RegisterSlot("event", RecordingSlot, &b, 1);

	BitStream parameters;
	parameters.Write(1);
	Packet packet;
	rpc.InvokeSignal(rpc.GetLocalSlot("event"), &parameters, &packet);
	const std::vector<std::string> expected = {"a:1"};
	EXPECT_EQ(expected, log.calls);
}

TEST(RPCFunctions, RegisterRejectsDuplicatesAndUnregisterReportsRemoval)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	int calls = 0;
	EXPECT_TRUE(rpc.RegisterFunction("f", CountingFunction, &calls));
	EXPECT_FALSE(rpc.RegisterFunction("f", CountingFunction, &calls));
	EXPECT_TRUE(rpc.UnregisterFunction("f"));
	EXPECT_FALSE(rpc.UnregisterFunction("f"));
	EXPECT_TRUE(rpc.RegisterFunction("f", CountingFunction, &calls));
}

TEST(RPCFunctions, RegistrationsOwnTheirIdentifierAfterCallerReusesTheBuffer)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	int calls = 0;
	const std::string original = std::string(160, 'a') + "%handler";
	std::string buffer = original;
	ASSERT_TRUE(rpc.RegisterFunction(buffer.c_str(), CountingFunction, &calls));
	rpc.RegisterLocalCallback(buffer.c_str(), ID_USER_PACKET_ENUM);
	rpc.RegisterSlot(buffer.c_str(), CountingFunction, &calls, 0);
	buffer[0] = 'b';

	EXPECT_EQ(rpc.GetLocalSlot(buffer.c_str()), nullptr);
	EXPECT_NE(rpc.GetLocalSlot(original.c_str()), nullptr);
	EXPECT_TRUE(rpc.UnregisterSlot(original.c_str()));
	EXPECT_TRUE(rpc.UnregisterLocalCallback(original.c_str(), ID_USER_PACKET_ENUM));
	EXPECT_TRUE(rpc.UnregisterFunction(original.c_str()));
}

TEST(RPCFunctions, NullAndEmptyRegistrationNamesShareOneKey)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	int calls = 0;
	ASSERT_TRUE(rpc.RegisterFunction(nullptr, CountingFunction, &calls));
	EXPECT_FALSE(rpc.RegisterFunction("", CountingFunction, &calls));
	EXPECT_TRUE(rpc.UnregisterFunction(""));
}

TEST(RPCFunctions, UnregisterLocalCallbackReportsEveryRemoval)
{
	RakPeer peer;
	RPC &rpc = peer.GetRPC();
	rpc.RegisterLocalCallback("onLost", ID_CONNECTION_LOST);
	rpc.RegisterLocalCallback("alsoOnLost", ID_CONNECTION_LOST);
	rpc.RegisterLocalCallback("onLost", ID_CONNECTION_LOST); // duplicate, ignored

	EXPECT_FALSE(rpc.UnregisterLocalCallback("unknown", ID_CONNECTION_LOST));
	EXPECT_FALSE(rpc.UnregisterLocalCallback("onLost", ID_DISCONNECTION_NOTIFICATION));
	EXPECT_TRUE(rpc.UnregisterLocalCallback("onLost", ID_CONNECTION_LOST));
	EXPECT_FALSE(rpc.UnregisterLocalCallback("onLost", ID_CONNECTION_LOST));
	EXPECT_TRUE(rpc.UnregisterLocalCallback("alsoOnLost", ID_CONNECTION_LOST));
	EXPECT_FALSE(rpc.UnregisterLocalCallback("alsoOnLost", ID_CONNECTION_LOST));
}

class RPCNetwork : public ::testing::Test
{
protected:
	SimulatedNetwork net;
	RakPeer *client = nullptr;
	RakPeer *server = nullptr;

	void SetUp() override
	{
		client = net.AddPeer(1);
		server = net.AddPeer(1, 1);
		ASSERT_NE(nullptr, client);
		ASSERT_NE(nullptr, server);
		ASSERT_TRUE(net.Connect(client, server));
	}
};

TEST_F(RPCNetwork, RemoteSignalReachesSlotsInPriorityOrder)
{
	CallLog log;
	SlotContext low{&log, "low"}, high{&log, "high"};
	server->GetRPC().RegisterSlot("event", RecordingSlot, &low, 0);
	server->GetRPC().RegisterSlot("event", RecordingSlot, &high, 9);

	BitStream parameters;
	parameters.Write(5);
	client->GetRPC().Signal("event", &parameters, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false, false);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return log.calls.size() == 2; }, 2000));
	const std::vector<std::string> expected = {"high:5", "low:5"};
	EXPECT_EQ(expected, log.calls);
}

TEST_F(RPCNetwork, LocalSignalInvokesOwnSlots)
{
	CallLog log;
	SlotContext ctx{&log, "self"};
	server->GetRPC().RegisterSlot("event", RecordingSlot, &ctx, 0);
	BitStream parameters;
	parameters.Write(3);
	server->GetRPC().Signal("event", &parameters, Priority::High, Reliability::ReliableOrdered, 0, client->GetMyGUID(), false, true);
	const std::vector<std::string> expected = {"self:3"};
	EXPECT_EQ(expected, log.calls);
}

// Names are registered and looked up verbatim, including percent signs.
TEST_F(RPCNetwork, FunctionNameWithPercentIsMatchedVerbatim)
{
	int calls = 0;
	ASSERT_TRUE(server->GetRPC().RegisterFunction("odd%s%n%dname", CountingFunction, &calls));
	client->GetRPC().Call("odd%s%n%dname", nullptr, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return calls == 1; }, 2000));

	// An unknown name, '%' included, comes back as an error naming it.
	client->GetRPC().Call("missing%s", nullptr, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_RPC_REMOTE_ERROR, 2000));
	const SimulatedNetwork::Message *error = net.Find(client, ID_RPC_REMOTE_ERROR);
	ASSERT_NE(nullptr, error);
	ASSERT_GE(error->data.size(), 3u);
	EXPECT_EQ(RPC_ERROR_FUNCTION_NOT_REGISTERED, error->data[1]);
	EXPECT_EQ(std::string("missing%s"), std::string((const char *) error->data.data() + 2));
}

TEST_F(RPCNetwork, LocalCallbackRunsRegisteredFunctionForItsMessageId)
{
	int calls = 0;
	ASSERT_TRUE(server->GetRPC().RegisterFunction("onUser", CountingFunction, &calls));
	server->GetRPC().RegisterLocalCallback("onUser", ID_USER_PACKET_ENUM);

	BitStream message;
	message.Write((MessageID) ID_USER_PACKET_ENUM);
	client->Send(&message, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_USER_PACKET_ENUM, 2000));
	EXPECT_EQ(1, calls);
}
