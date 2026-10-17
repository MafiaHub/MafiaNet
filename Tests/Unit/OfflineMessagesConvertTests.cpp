/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Messages to systems you are not connected to: SetOfflinePingResponse /
 *  GetOfflinePingResponse, AdvertiseSystem and the unconnected Ping, over the
 *  in-process SimulatedNetwork.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
	const char kPingData[] = "Offline Ping Data";
	const char kAdvertiseData[] = "hello world";

	void HostOf(SimulatedNetwork &net, const RakPeer *peer, char (&host)[64])
	{
		net.AddressOf(peer).ToString(false, host, sizeof(host));
	}
}

TEST(OfflineMessagesConvert, GetOfflinePingResponseReturnsWhatWasSet)
{
	SimulatedNetwork net;
	RakPeer *peer = net.AddPeer(1, 1);

	peer->SetOfflinePingResponse(kPingData, (unsigned int) sizeof(kPingData));
	char *data = nullptr;
	unsigned int length = 0;
	peer->GetOfflinePingResponse(&data, &length);
	ASSERT_NE(nullptr, data);
	EXPECT_EQ(sizeof(kPingData), length);
	EXPECT_STREQ(kPingData, data);
}

TEST(OfflineMessagesConvert, AdvertiseSystemDeliversItsDataUnconnected)
{
	SimulatedNetwork net;
	RakPeer *peer1 = net.AddPeer(1, 1);
	RakPeer *peer2 = net.AddPeer(1);

	char host[64];
	HostOf(net, peer2, host);
	ASSERT_TRUE(peer1->AdvertiseSystem(host, net.AddressOf(peer2).GetPort(), kAdvertiseData, (int) sizeof(kAdvertiseData)));

	ASSERT_TRUE(net.RunUntilReceived(peer2, ID_ADVERTISE_SYSTEM, 1000));
	const SimulatedNetwork::Message *advert = net.Find(peer2, ID_ADVERTISE_SYSTEM);
	ASSERT_EQ(1u + sizeof(kAdvertiseData), advert->data.size()) << "Got Advertise system with unexpected data";
	EXPECT_STREQ(kAdvertiseData, (const char *) advert->data.data() + 1);
	EXPECT_EQ(net.AddressOf(peer1), advert->systemAddress);
	EXPECT_EQ(peer1->GetMyGUID(), advert->guid);
	EXPECT_EQ(IS_NOT_CONNECTED, peer2->GetConnectionState(net.AddressOf(peer1)));
}

// The pong carries the ping's own send time back, then the responder's offline
// ping response. With a virtual clock both are exact.
TEST(OfflineMessagesConvert, UnconnectedPingIsAnsweredWithTheOfflinePingResponse)
{
	SimulatedNetwork net;
	RakPeer *peer1 = net.AddPeer(1, 1);
	RakPeer *peer2 = net.AddPeer(1);
	peer1->SetOfflinePingResponse(kPingData, (unsigned int) sizeof(kPingData));

	char host[64];
	HostOf(net, peer1, host);
	const TimeMS sentAt = (TimeMS) (net.Now() / 1000);
	ASSERT_TRUE(peer2->Ping(host, net.AddressOf(peer1).GetPort(), false));

	ASSERT_TRUE(net.RunUntilReceived(peer2, ID_UNCONNECTED_PONG, 1000));
	const SimulatedNetwork::Message *pong = net.Find(peer2, ID_UNCONNECTED_PONG);
	ASSERT_EQ(1u + sizeof(TimeMS) + sizeof(kPingData), pong->data.size());

	// Written with BitStream (network byte order), so read it the same way.
	BitStream bs(const_cast<unsigned char *>(pong->data.data()), (unsigned int) pong->data.size(), false);
	bs.IgnoreBytes(1);
	TimeMS echoedTime = 0;
	ASSERT_TRUE(bs.Read(echoedTime));
	EXPECT_EQ(sentAt, echoedTime);
	EXPECT_STREQ(kPingData, (const char *) pong->data.data() + 1 + sizeof(TimeMS)) << "Received wrong offline ping response";
	EXPECT_EQ(net.AddressOf(peer1), pong->systemAddress);
	EXPECT_EQ(peer1->GetMyGUID(), pong->guid);
}

// A responder with no offline ping response still answers, with no data.
TEST(OfflineMessagesConvert, UnconnectedPingWithoutAResponseGetsAnEmptyPong)
{
	SimulatedNetwork net;
	RakPeer *peer1 = net.AddPeer(1, 1);
	RakPeer *peer2 = net.AddPeer(1);

	char host[64];
	HostOf(net, peer1, host);
	ASSERT_TRUE(peer2->Ping(host, net.AddressOf(peer1).GetPort(), false));

	ASSERT_TRUE(net.RunUntilReceived(peer2, ID_UNCONNECTED_PONG, 1000));
	EXPECT_EQ(1u + sizeof(TimeMS), net.Find(peer2, ID_UNCONNECTED_PONG)->data.size());
}

// The legacy sequence: advertise, and answer the advert with a ping.
TEST(OfflineMessagesConvert, AdvertiseSystemAndOfflinePing)
{
	SimulatedNetwork net;
	RakPeer *peer1 = net.AddPeer(1, 1);
	RakPeer *peer2 = net.AddPeer(1);
	peer1->SetOfflinePingResponse(kPingData, (unsigned int) sizeof(kPingData));

	char host[64];
	HostOf(net, peer2, host);
	ASSERT_TRUE(peer1->AdvertiseSystem(host, net.AddressOf(peer2).GetPort(), kAdvertiseData, (int) sizeof(kAdvertiseData)));
	ASSERT_TRUE(net.RunUntilReceived(peer2, ID_ADVERTISE_SYSTEM, 1000)) << "Never got proper offline data";
	EXPECT_STREQ(kAdvertiseData, (const char *) net.Find(peer2, ID_ADVERTISE_SYSTEM)->data.data() + 1);

	// Reply to whoever advertised, at the address the advert came from.
	const SystemAddress advertiser = net.Find(peer2, ID_ADVERTISE_SYSTEM)->systemAddress;
	char advertiserHost[64];
	advertiser.ToString(false, advertiserHost, sizeof(advertiserHost));
	ASSERT_TRUE(peer2->Ping(advertiserHost, advertiser.GetPort(), false));

	ASSERT_TRUE(net.RunUntilReceived(peer2, ID_UNCONNECTED_PONG, 1000)) << "Never got proper ping data";
	EXPECT_STREQ(kPingData, (const char *) net.Find(peer2, ID_UNCONNECTED_PONG)->data.data() + 1 + sizeof(TimeMS));
}
