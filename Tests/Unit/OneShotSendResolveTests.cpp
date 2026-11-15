/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  One-shot unconnected sends (Ping, AdvertiseSystem / SendOutOfBand, SendTTL)
 *  on a dual-stack socket have no Happy Eyeballs race to fall back on, so a
 *  host name that resolves to both families must go to its IPv4 address, as
 *  it did when IPv4 sockets were the default; only a name without an IPv4
 *  address goes to IPv6. On the SimulatedNetwork, with names resolved through
 *  its own table: the IPv6 answer listed first leads nowhere.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
const char *const kHost = "game.test";

// 2001:db8::99 is no simulated peer: anything sent there is lost.
SystemAddress Nowhere6()
{
	SystemAddress a;
	EXPECT_TRUE(a.FromString("2001:db8::99", 0, 6));
	return a;
}
} // namespace

TEST(OneShotSendResolve, PingPrefersIpv4AnswerOnDualStackSocket)
{
	SimulatedNetwork net;
	RakPeer *dual = net.AddPeerDualStack(1);
	RakPeer *v4 = net.AddPeer(1);
	net.AddHostName(kHost, {Nowhere6(), net.AddressOf(v4)});

	ASSERT_TRUE(dual->Ping(kHost, net.AddressOf(v4).GetPort(), false));
	EXPECT_TRUE(net.RunUntilReceived(dual, ID_UNCONNECTED_PONG, 2000));
}

TEST(OneShotSendResolve, AdvertiseSystemPrefersIpv4AnswerOnDualStackSocket)
{
	SimulatedNetwork net;
	RakPeer *dual = net.AddPeerDualStack(1);
	RakPeer *v4 = net.AddPeer(1);
	net.AddHostName(kHost, {Nowhere6(), net.AddressOf(v4)});

	const char data[] = "hi";
	ASSERT_TRUE(dual->AdvertiseSystem(kHost, net.AddressOf(v4).GetPort(), data, (int) sizeof(data)));
	EXPECT_TRUE(net.RunUntilReceived(v4, ID_ADVERTISE_SYSTEM, 2000));
}

// SendTTL takes the same path (ResolveOneShotTarget) but only on an OS socket,
// so it is covered by DualStackDefaultLive.SendTtlToHostNamePrefersIpv4.

TEST(OneShotSendResolve, NameWithoutIpv4AnswerUsesIpv6)
{
	SimulatedNetwork net;
	RakPeer *dual = net.AddPeerDualStack(1);
	RakPeer *v6 = net.AddPeerV6(1);
	net.AddHostName(kHost, {net.AddressOf(v6)});

	ASSERT_TRUE(dual->Ping(kHost, net.AddressOf(v6).GetPort(), false));
	EXPECT_TRUE(net.RunUntilReceived(dual, ID_UNCONNECTED_PONG, 2000));
}

TEST(OneShotSendResolve, UnresolvableNameIsNotSent)
{
	SimulatedNetwork net;
	RakPeer *dual = net.AddPeerDualStack(1);
	EXPECT_FALSE(dual->Ping("unknown.test", 1234, false));
	EXPECT_FALSE(dual->AdvertiseSystem("unknown.test", 1234, "x", 1));
}
