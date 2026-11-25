/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Two peers on the SimulatedNetwork connect to each other at the same instant
 *  (agreed through an unconnected ping/pong). Crossing handshakes have broken
 *  RakNet in the past; both sides must still end up with one working link.
 *
 *  With encryption (the default) each handshake derives its own session keys,
 *  so the two peers must also agree on which handshake's keys they use: the
 *  one initiated by the peer with the lower RakNetGUID, whatever the order in
 *  which the handshake messages cross.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/session_crypto.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;

namespace
{
const int kRounds = 5;

void PingServer(SimulatedNetwork &net, RakPeer *client, const RakPeer *server)
{
	const SystemAddress target = net.AddressOf(server);
	char host[64];
	target.ToString(false, host, sizeof(host));
	ASSERT_TRUE(client->Ping(host, target.GetPort(), false));
}

bool GotConnectionNotification(SimulatedNetwork &net, const RakPeer *peer)
{
	return net.Received(peer, ID_NEW_INCOMING_CONNECTION) || net.Received(peer, ID_CONNECTION_REQUEST_ACCEPTED);
}

void RunCrossConnectRounds(unsigned short clientMaxIncoming)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1, clientMaxIncoming);
	const SystemAddress serverAddress = net.AddressOf(server);
	const SystemAddress clientAddress = net.AddressOf(client);

	for (int round = 0; round < kRounds; ++round)
	{
		SCOPED_TRACE(testing::Message() << "round " << round);
		net.Inbox(server).clear();
		net.Inbox(client).clear();

		// Ping/pong: the server learns the client's address, the client
		// gets its own send time echoed back.
		const TimeMS pingSentAt = GetTimeMS();
		ASSERT_NO_FATAL_FAILURE(PingServer(net, client, server));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return net.Received(server, ID_UNCONNECTED_PING) && net.Received(client, ID_UNCONNECTED_PONG); }, 1000));
		EXPECT_EQ(clientAddress, net.Find(server, ID_UNCONNECTED_PING)->systemAddress);
		{
			const SimulatedNetwork::Message *pong = net.Find(client, ID_UNCONNECTED_PONG);
			BitStream bs(const_cast<unsigned char *>(pong->data.data()), (unsigned int) pong->data.size(), false);
			bs.IgnoreBytes(1);
			TimeMS echoed = 0;
			ASSERT_TRUE(bs.Read(echoed));
			EXPECT_EQ(pingSentAt, echoed);
		}

		// Both sides wait out the agreed second and connect in the same tick.
		net.Step(1000);
		EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(server, client));
		EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));

		net.RunUntil([&]
		             { return client->GetConnectionState(serverAddress) == IS_CONNECTED &&
			                  server->GetConnectionState(clientAddress) == IS_CONNECTED; }, 2000);
		EXPECT_TRUE(GotConnectionNotification(net, server) || GotConnectionNotification(net, client))
		    << "neither side was told about a connection";
		EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(serverAddress));
		EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(clientAddress));
		EXPECT_EQ(server->GetMyGUID(), client->GetGuidFromSystemAddress(serverAddress));
		EXPECT_EQ(client->GetMyGUID(), server->GetGuidFromSystemAddress(clientAddress));
		ASSERT_FALSE(testing::Test::HasFailure());

		// Tear down for the next round.
		client->CancelConnectionAttempt(serverAddress);
		server->CancelConnectionAttempt(clientAddress);
		server->CloseConnection(server->GetSystemAddressFromIndex(0), true, 0);
		client->CloseConnection(client->GetSystemAddressFromIndex(0), true, 0);
		net.Step(1000);
		for (const ConnectionState state : {client->GetConnectionState(serverAddress), server->GetConnectionState(clientAddress)})
			ASSERT_TRUE(state == IS_DISCONNECTED || state == IS_NOT_CONNECTED) << "state " << state;
	}
}
}

TEST(CrossConnectionConvertSim, SimultaneousConnectWhenOnlyOneSideAcceptsIncoming)
{
	RunCrossConnectRounds(0);
}

TEST(CrossConnectionConvertSim, SimultaneousConnectWhenBothSidesAcceptIncoming)
{
	RunCrossConnectRounds(1);
}

namespace
{
const MessageID ID_TEST_PAYLOAD = ID_USER_PACKET_ENUM;

// Two started peers that accept one incoming connection each, named by GUID order.
struct GuidOrderedPair
{
	RakPeer *lower;
	RakPeer *higher;
};

GuidOrderedPair AddGuidOrderedPair(SimulatedNetwork &net)
{
	RakPeer *a = net.AddPeer(1, 1);
	RakPeer *b = net.AddPeer(1, 1);
	if (a->GetMyGUID().g < b->GetMyGUID().g)
		return {a, b};
	return {b, a};
}

bool BothConnected(const RakPeer *a, const SystemAddress &aToB, const RakPeer *b, const SystemAddress &bToA)
{
	return const_cast<RakPeer *>(a)->GetConnectionState(aToB) == IS_CONNECTED &&
	       const_cast<RakPeer *>(b)->GetConnectionState(bToA) == IS_CONNECTED;
}

size_t CountOf(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	size_t n = 0;
	for (const auto &message : net.Inbox(peer))
		n += message.id == id;
	return n;
}

std::vector<uint32_t> NumbersReceived(SimulatedNetwork &net, const RakPeer *peer)
{
	std::vector<uint32_t> numbers;
	for (const auto &message : net.Inbox(peer))
	{
		if (message.id != ID_TEST_PAYLOAD || message.data.size() < 5)
			continue;
		BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
		bs.IgnoreBytes(1);
		uint32_t number = 0;
		bs.Read(number);
		numbers.push_back(number);
	}
	return numbers;
}

// Both sides hold the two halves of one session: what one sends with, the other opens with.
void ExpectPairedKeys(RakPeer *a, const SystemAddress &aToB, RakPeer *b, const SystemAddress &bToA)
{
	SessionKeys aKeys, bKeys;
	ASSERT_TRUE(a->GetSessionKeysForTesting(aToB, &aKeys));
	ASSERT_TRUE(b->GetSessionKeysForTesting(bToA, &bKeys));
	EXPECT_EQ(0, std::memcmp(aKeys.send, bKeys.recv, sizeof(aKeys.send))) << "the two sides use keys from different handshakes";
	EXPECT_EQ(0, std::memcmp(aKeys.recv, bKeys.send, sizeof(aKeys.recv))) << "the two sides use keys from different handshakes";
}

// Each side sends `count` ReliableOrdered messages; all arrive, in order, and nothing failed to open.
void ExpectBothDirectionsDeliver(SimulatedNetwork &net, RakPeer *a, const SystemAddress &aToB, RakPeer *b, const SystemAddress &bToA, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		for (RakPeer *from : {a, b})
		{
			BitStream bs;
			bs.Write(ID_TEST_PAYLOAD);
			bs.Write(i);
			bs.Write((uint32_t) 0xC0FFEE00u + i);
			ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, from == a ? aToB : bToA, false));
		}
	}
	ASSERT_TRUE(net.RunUntil([&]
	                         { return NumbersReceived(net, a).size() >= count && NumbersReceived(net, b).size() >= count; }, 10000));
	std::vector<uint32_t> expected(count);
	for (uint32_t i = 0; i < count; ++i)
		expected[i] = i;
	EXPECT_EQ(expected, NumbersReceived(net, a));
	EXPECT_EQ(expected, NumbersReceived(net, b));
	EXPECT_EQ(0u, a->GetStatistics(aToB)->droppedUnauthenticatedDatagrams);
	EXPECT_EQ(0u, b->GetStatistics(bToA)->droppedUnauthenticatedDatagrams);
}

// The lower GUID's handshake is the one in use: the lower peer sent ID_CONNECTION_REQUEST (so it was told
// ID_CONNECTION_REQUEST_ACCEPTED) and the higher peer accepted it as an incoming connection.
void ExpectLowerGuidHandshakeWon(SimulatedNetwork &net, const GuidOrderedPair &p)
{
	EXPECT_EQ(1u, CountOf(net, p.lower, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(0u, CountOf(net, p.lower, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(1u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_CONNECTION_REQUEST_ACCEPTED));
}

// Counts ID_OPEN_CONNECTION_REPLY_2 per sender.
struct Reply2Counter
{
	std::vector<SystemAddress> senders;
	void Attach(SimulatedNetwork &net)
	{
		net.SetTap([this](const SystemAddress &from, const SystemAddress &, const char *data, int length)
		           {
			           if (length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_2)
				           senders.push_back(from); });
	}
	size_t From(const SystemAddress &address) const
	{
		size_t n = 0;
		for (const SystemAddress &s : senders)
			n += s == address;
		return n;
	}
};

void RunSimultaneousEncryptedConnect(bool lowerStartsFirst)
{
	SimulatedNetwork net;
	Reply2Counter replies;
	replies.Attach(net);
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);
	ASSERT_EQ(EncryptionMode::Required, p.lower->GetEncryptionMode());

	RakPeer *first = lowerStartsFirst ? p.lower : p.higher;
	RakPeer *second = lowerStartsFirst ? p.higher : p.lower;
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(first, second));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(second, first));

	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 2000));
	ExpectLowerGuidHandshakeWon(net, p);
	// The higher peer's attempt is dropped quietly: it is now the accepting side of the same connection.
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_ALREADY_CONNECTED));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_CONNECTION_ATTEMPT_FAILED));
	// The lower peer never answers the higher peer's handshake while its own is under way, so no keys of the
	// losing handshake are ever derived.
	EXPECT_EQ(0u, replies.From(lowerAddress));
	ExpectPairedKeys(p.lower, higherAddress, p.higher, lowerAddress);
	ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 50);
}
} // namespace

TEST(CrossConnectionConvert, SimultaneousConnectEncryptedBothDirectionsDeliver)
{
	RunSimultaneousEncryptedConnect(true);
}

TEST(CrossConnectionConvert, SimultaneousConnectEncryptedBothDirectionsDeliverWhenHigherGuidStartsFirst)
{
	RunSimultaneousEncryptedConnect(false);
}

// Per-direction latency, jitter and loss decide which handshake message reaches which peer first. Every
// order must end with the lower GUID's keys on both sides and nothing sealed under any other key.
TEST(CrossConnectionConvert, LowerGuidHandshakeWinsUnderEveryDeliveryOrder)
{
	struct Case
	{
		const char *name;
		uint32_t lowerToHigherMs;
		uint32_t higherToLowerMs;
		uint32_t jitterMs;
		int lossPercent;
		uint32_t seed;
	};
	const Case cases[] = {
	    {"symmetric", 1, 1, 0, 0, 1},
	    {"higher's REQUEST_2 lands long before the lower's", 150, 1, 0, 0, 1},
	    {"lower's REQUEST_2 lands long before the higher's", 1, 150, 0, 0, 1},
	    {"slightly slower towards the higher peer", 3, 1, 0, 0, 1},
	    {"slightly slower towards the lower peer", 1, 3, 0, 0, 1},
	    {"jitter reorders", 5, 5, 20, 0, 7},
	    {"loss and jitter, seed 2", 5, 5, 10, 10, 2},
	    {"loss and jitter, seed 3", 5, 5, 10, 10, 3},
	    {"loss and jitter, seed 4", 5, 5, 10, 10, 4},
	};
	for (const Case &c : cases)
	{
		SCOPED_TRACE(c.name);
		SimulatedNetwork net(c.seed);
		const GuidOrderedPair p = AddGuidOrderedPair(net);
		const SystemAddress lowerAddress = net.AddressOf(p.lower);
		const SystemAddress higherAddress = net.AddressOf(p.higher);
		SimulatedNetwork::LinkRule toHigher, toLower;
		toHigher.delayMs = c.lowerToHigherMs;
		toLower.delayMs = c.higherToLowerMs;
		toHigher.jitterMs = toLower.jitterMs = c.jitterMs;
		toHigher.lossPercent = toLower.lossPercent = c.lossPercent;
		net.SetLink(p.lower, p.higher, toHigher);
		net.SetLink(p.higher, p.lower, toLower);

		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 10000, 1));
		ExpectLowerGuidHandshakeWon(net, p);
		// Retries under loss and jitter must not fail either side's attempt along the way.
		EXPECT_EQ(0u, CountOf(net, p.higher, ID_CONNECTION_ATTEMPT_FAILED));
		EXPECT_EQ(0u, CountOf(net, p.lower, ID_CONNECTION_ATTEMPT_FAILED));
		ExpectPairedKeys(p.lower, higherAddress, p.higher, lowerAddress);
		ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 20);
	}
}

// The REPLY_2 guard: the higher peer has answered the lower peer's handshake (and holds its keys) while its
// own attempt is still queued. A REPLY_2 to that attempt -- here a stale or forged one -- must not touch the
// installed keys, must not make the higher peer send ID_CONNECTION_REQUEST under them, and must leave its
// attempt queued until the winning handshake completes.
TEST(CrossConnectionConvert, Reply2ToTheLosingHandshakeIsIgnored)
{
	SimulatedNetwork net;
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);
	// Slow the way back to the lower peer, so the higher peer sits answering for a while before the lower
	// peer's ID_CONNECTION_REQUEST can reach it.
	SimulatedNetwork::LinkRule slow;
	slow.delayMs = 40;
	net.SetLink(p.higher, p.lower, slow);
	bool higherAnswered = false;
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *data, int length)
	           { higherAnswered = higherAnswered || (from == higherAddress && length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_2); });

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return higherAnswered; }, 500, 1));
	// Precondition: keys of the lower peer's handshake installed, the higher peer's own attempt still queued.
	SessionKeys before;
	ASSERT_TRUE(p.higher->GetSessionKeysForTesting(lowerAddress, &before));
	ASSERT_EQ(IS_PENDING, p.higher->GetConnectionState(lowerAddress));

	// ID_OPEN_CONNECTION_REPLY_2 from the lower peer's address to the higher peer's attempt: id | offline id (16)
	// | nonce echo (8) | server guid (8) | client address as seen (SystemAddress) | mtu (2) | server crypt info (41)
	// | identity block (1: none).
	// It echoes the attempt's nonce, so it gets past the echo check to the guard under test.
	uint64_t attemptNonce = 0;
	ASSERT_TRUE(p.higher->GetPendingHandshakeNonceForTesting(lowerAddress, &attemptNonce));
	EphemeralKeyPair forgedPair;
	BitStream reply2;
	reply2.Write((MessageID) ID_OPEN_CONNECTION_REPLY_2);
	reply2.WriteAlignedBytes(RakPeer::GetOfflineMessageDataIdForTesting(), RakPeer::kOfflineMessageDataIdBytes);
	reply2.Write(attemptNonce);
	reply2.Write(p.lower->GetMyGUID());
	reply2.Write(higherAddress);
	reply2.Write((uint16_t) 1200);
	forgedPair.MakeCryptInfo().Serialize(&reply2);
	IdentityBlock().Serialize(&reply2);
	net.InjectDatagram(lowerAddress, p.higher, reply2.GetData(), (int) reply2.GetNumberOfBytesUsed());
	net.Step(5);

	SessionKeys after;
	ASSERT_TRUE(p.higher->GetSessionKeysForTesting(lowerAddress, &after));
	EXPECT_EQ(0, std::memcmp(before.send, after.send, sizeof(before.send)));
	EXPECT_EQ(0, std::memcmp(before.recv, after.recv, sizeof(before.recv)));
	EXPECT_EQ(IS_PENDING, p.higher->GetConnectionState(lowerAddress)) << "the REPLY_2 was taken as an answer to the higher peer's attempt";

	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 2000));
	ExpectLowerGuidHandshakeWon(net, p);
	ExpectPairedKeys(p.lower, higherAddress, p.higher, lowerAddress);
	ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 20);
}

// Only one side takes incoming connections: the other side's handshake is the only one that can complete,
// whichever GUID is lower, and it completes without waiting out a connection-attempt retry interval.
TEST(CrossConnectionConvert, OnlyTheLowerGuidAcceptsIncoming)
{
	SimulatedNetwork net;
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	p.higher->SetMaximumIncomingConnections(0);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 500));
	EXPECT_EQ(1u, CountOf(net, p.higher, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(1u, CountOf(net, p.lower, ID_NEW_INCOMING_CONNECTION));
	ExpectPairedKeys(p.lower, higherAddress, p.higher, lowerAddress);
	ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 20);
}

TEST(CrossConnectionConvert, OnlyTheHigherGuidAcceptsIncoming)
{
	SimulatedNetwork net;
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	p.lower->SetMaximumIncomingConnections(0);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 500));
	ExpectLowerGuidHandshakeWon(net, p);
	ExpectPairedKeys(p.lower, higherAddress, p.higher, lowerAddress);
	ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 20);
}

// Without encryption there are no keys to agree on: the crossing handshakes keep converging as before.
TEST(CrossConnectionConvert, PlaintextPeersStillConnectToEachOther)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 2000));
	ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 20);
}

// Simultaneous plaintext attempts select one accepting peer, including under loss and reordering.
TEST(CrossConnectionConvert, PlaintextCrossingConnectsUnderEveryDeliveryOrder)
{
	struct Case
	{
		const char *name;
		uint32_t lowerToHigherMs;
		uint32_t higherToLowerMs;
		uint32_t jitterMs;
		int lossPercent;
		uint32_t seed;
	};
	const Case cases[] = {
	    {"symmetric", 1, 1, 0, 0, 1},
	    {"slower towards the higher peer", 150, 1, 0, 0, 1},
	    {"slower towards the lower peer", 1, 150, 0, 0, 1},
	    {"slightly slower towards the higher peer", 3, 1, 0, 0, 1},
	    {"slightly slower towards the lower peer", 1, 3, 0, 0, 1},
	    {"jitter reorders", 5, 5, 20, 0, 7},
	    {"loss and jitter, seed 2", 5, 5, 10, 10, 2},
	    {"loss and jitter, seed 3", 5, 5, 10, 10, 3},
	    {"loss and jitter, seed 4", 5, 5, 10, 10, 4},
	};
	for (const Case &c : cases)
	{
		SCOPED_TRACE(c.name);
		SimulatedNetwork net(c.seed);
		net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
		const GuidOrderedPair p = AddGuidOrderedPair(net);
		const SystemAddress lowerAddress = net.AddressOf(p.lower);
		const SystemAddress higherAddress = net.AddressOf(p.higher);
		SimulatedNetwork::LinkRule toHigher, toLower;
		toHigher.delayMs = c.lowerToHigherMs;
		toLower.delayMs = c.higherToLowerMs;
		toHigher.jitterMs = toLower.jitterMs = c.jitterMs;
		toHigher.lossPercent = toLower.lossPercent = c.lossPercent;
		net.SetLink(p.lower, p.higher, toHigher);
		net.SetLink(p.higher, p.lower, toLower);

		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 10000, 1));
		net.Step(500);
		EXPECT_EQ(1u, CountOf(net, p.higher, ID_SESSION_CONFIG_REQUEST));
		EXPECT_EQ(0u, CountOf(net, p.lower, ID_SESSION_CONFIG_REQUEST));
		for (const RakPeer *peer : {p.lower, p.higher})
		{
			EXPECT_EQ(0u, CountOf(net, peer, ID_CONNECTION_ATTEMPT_FAILED));
			EXPECT_EQ(1u, CountOf(net, peer, ID_CONNECTION_REQUEST_ACCEPTED) + CountOf(net, peer, ID_NEW_INCOMING_CONNECTION))
			    << "a Connect() was answered other than by exactly one connection packet";
		}
		ExpectBothDirectionsDeliver(net, p.lower, higherAddress, p.higher, lowerAddress, 20);
	}
}

// The higher peer's own Connect() was absorbed into the lower peer's connection. If that connection dies before
// it is reported (here: the lower peer vanishes right after its ID_CONNECTION_REQUEST was accepted), the higher
// peer's Connect() still gets its answer: ID_CONNECTION_ATTEMPT_FAILED, exactly once.
TEST(CrossConnectionConvert, AbsorbedAttemptFailsWhenTheWinningConnectionDiesBeforeCompleting)
{
	SimulatedNetwork net;
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	// Slow the way back to the lower peer: the window between "higher peer accepted the ID_CONNECTION_REQUEST"
	// and "lower peer's ID_NEW_INCOMING_CONNECTION arrived" is then wide enough to cut the link in.
	SimulatedNetwork::LinkRule slow;
	slow.delayMs = 30;
	net.SetLink(p.higher, p.lower, slow);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
	// The higher peer's attempt is pending until the winner's ID_CONNECTION_REQUEST opens, then absorbed.
	ASSERT_TRUE(net.RunUntil([&]
	                         { return p.higher->GetConnectionState(lowerAddress) != IS_PENDING; }, 1000, 1));
	ASSERT_NE(IS_CONNECTED, p.higher->GetConnectionState(lowerAddress)) << "precondition: the connection is not complete yet";
	ASSERT_EQ(0u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));

	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetLinkBothWays(p.lower, p.higher, dark);
	net.Step(20000);

	EXPECT_EQ(1u, CountOf(net, p.higher, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_CONNECTION_LOST));
	EXPECT_NE(IS_CONNECTED, p.higher->GetConnectionState(lowerAddress));
}

// Before the winner's ID_CONNECTION_REQUEST arrives the higher peer's attempt is still its own: if the lower peer
// vanishes then, the attempt runs out of retries and fails, exactly once.
TEST(CrossConnectionConvert, PendingAttemptFailsWhenTheLowerPeerVanishesAfterBeingAnswered)
{
	SimulatedNetwork net;
	const GuidOrderedPair p = AddGuidOrderedPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);
	bool higherAnswered = false;
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *data, int length)
	           { higherAnswered = higherAnswered || (from == higherAddress && length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_2); });

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.lower, p.higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(p.higher, p.lower));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return higherAnswered; }, 1000, 1));
	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetLinkBothWays(p.lower, p.higher, dark);
	ASSERT_EQ(IS_PENDING, p.higher->GetConnectionState(lowerAddress));
	net.Step(30000);

	EXPECT_EQ(1u, CountOf(net, p.higher, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));
	EXPECT_NE(IS_CONNECTED, p.higher->GetConnectionState(lowerAddress));
}
