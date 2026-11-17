/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Router2 on the simulated network, driven by hand-made replies from relays that run no Router2:
// what a sender does with the ping and load figures relays report.

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/router2.h"

using namespace MafiaNet;

namespace
{
// Router2's internal sub-identifiers (router2.cpp).
constexpr unsigned char kQueryForwarding = 0;
constexpr unsigned char kReplyForwarding = 1;
constexpr unsigned char kRequestForwarding = 2;

class Router2Sender : public ::testing::Test
{
protected:
	void SetUp() override
	{
		sender = net.AddPeer(8, 8);
		ASSERT_NE(sender, nullptr);
		sender->AttachPlugin(&router);
		for (RakPeer *&relay : relays)
		{
			relay = net.AddPeer(8, 8);
			ASSERT_NE(relay, nullptr);
			ASSERT_TRUE(net.Connect(relay, sender));
		}
	}
	void TearDown() override
	{
		if (sender)
			sender->DetachPlugin(&router);
	}

	bool ReceivedInternal(const RakPeer *peer, unsigned char subId)
	{
		for (const SimulatedNetwork::Message &m : net.Inbox(peer))
			if (m.id == ID_ROUTER_2_INTERNAL && m.data.size() >= 2 && m.data[1] == subId)
				return true;
		return false;
	}

	void ReplyCanForward(RakPeer *relay, RakNetGUID endpoint, unsigned short ping, unsigned short usedEntries)
	{
		BitStream bs;
		bs.Write((MessageID) ID_ROUTER_2_INTERNAL);
		bs.Write(kReplyForwarding);
		bs.Write(endpoint);
		bs.Write(true);
		bs.Write(ping);
		bs.Write(usedEntries);
		relay->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, sender->GetMyGUID(), false);
	}

	Router2 router;
	SimulatedNetwork net; // after the plugin: its peers are destroyed first
	RakPeer *sender = nullptr;
	RakPeer *relays[2] = {nullptr, nullptr};
};
} // namespace

TEST_F(Router2Sender, LoadWeightedPingDoesNotOverflowWithMaximalRelayFigures)
{
	// The weight ping * (usedEntries + 1) was an int product of peer-supplied 16-bit values: signed
	// overflow (UBSan reports it). Relay 0 reports a ping that, added to the sender's own ping to
	// it, is exactly 65536, with 65535 entries in use: the int product wrapped to 0 and made the
	// busiest relay look the best. Relay 1 is the real best choice.
	RakNetGUID endpoint;
	endpoint.g = 0x5eed5eed5eedull;
	router.EstablishRouting(endpoint);
	ASSERT_TRUE(net.RunUntil([this]
	                         { return ReceivedInternal(relays[0], kQueryForwarding) && ReceivedInternal(relays[1], kQueryForwarding); },
	                         2000));

	const int pingToRelay0 = sender->GetAveragePing(relays[0]->GetMyGUID());
	ASSERT_GE(pingToRelay0, 1);
	ASSERT_LE(pingToRelay0, 1000);
	ReplyCanForward(relays[0], endpoint, static_cast<unsigned short>(65536 - pingToRelay0), 65535);
	ReplyCanForward(relays[1], endpoint, 100, 0);

	ASSERT_TRUE(net.RunUntil([this]
	                         { return ReceivedInternal(relays[0], kRequestForwarding) || ReceivedInternal(relays[1], kRequestForwarding); },
	                         2000));
	EXPECT_TRUE(ReceivedInternal(relays[1], kRequestForwarding));
	EXPECT_FALSE(ReceivedInternal(relays[0], kRequestForwarding));
}
