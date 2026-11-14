/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/types.h"

#include <cstring>
#include <map>
#include <string>

using namespace MafiaNet;

TEST(SystemAddressOrdering, Ipv6LessIsIrreflexiveAndAsymmetric)
{
	const SystemAddress a("2001:db8::1", 1);
	const SystemAddress b("2001:db8::2", 1);
	EXPECT_TRUE(a < b);
	EXPECT_FALSE(b < a);
	EXPECT_FALSE(a < a);
	EXPECT_TRUE(b > a);
}

TEST(SystemAddressOrdering, MixedFamiliesAreStrictWeakOrder)
{
	const SystemAddress v4("10.0.0.1", 5);
	const SystemAddress v6("::1", 5);
	EXPECT_NE(v4 < v6, v6 < v4);

	std::map<SystemAddress, int> m;
	m[v4] = 1;
	m[v6] = 2;
	EXPECT_EQ(m.size(), 2u);
	ASSERT_NE(m.find(v4), m.end());
	ASSERT_NE(m.find(v6), m.end());
	EXPECT_EQ(m.find(v4)->second, 1);
	EXPECT_EQ(m.find(v6)->second, 2);
}

TEST(SystemAddressEquality, DifferentFamiliesNeverEqual)
{
	const SystemAddress v4("0.0.0.0", 7);
	const SystemAddress v6("::", 7);
	EXPECT_TRUE(v4 != v6);
	EXPECT_FALSE(v4 == v6);
	EXPECT_FALSE(v4.EqualsExcludingPort(v6));
	EXPECT_FALSE(v6.EqualsExcludingPort(v4));
}

TEST(SystemAddressParse, Ipv6WithoutPortKeepsPort)
{
	SystemAddress sa;
	sa.SetPortHostOrder(1234);
	ASSERT_TRUE(sa.FromString("2001:db8::5"));
	EXPECT_EQ(sa.GetPort(), 1234);
	EXPECT_EQ(sa.GetIPVersion(), 6);
}

TEST(SystemAddressParse, Ipv6WithPipePort)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("2001:db8::5|60000"));
	EXPECT_EQ(sa.GetPort(), 60000);
	EXPECT_STREQ(sa.ToString(true), "2001:db8::5|60000");
}

TEST(SystemAddressParse, InvalidStringReturnsFalse)
{
	SystemAddress sa;
	// RFC 6761: ".invalid" is guaranteed never to resolve.
	EXPECT_FALSE(sa.FromString("x.invalid"));
}

TEST(SystemAddressParse, SetBinaryAddressHostnameResolvesToIpv4)
{
	// SetBinaryAddress is the IPv4-only legacy parser. "localhost" resolves to
	// ::1 first on many hosts (RFC 6724 order); the IPv4 answer must still win
	// rather than the call silently keeping a stale address.
	SystemAddress sa("10.9.8.7", 4321);
	ASSERT_TRUE(sa.SetBinaryAddress("localhost"));
	EXPECT_EQ(sa.GetIPVersion(), 4);
	EXPECT_STREQ(sa.ToString(false), "127.0.0.1");
}

TEST(SystemAddressParse, OverlongStringDoesNotOverflow)
{
	SystemAddress sa;
	const std::string longHost(200, 'a');
	EXPECT_FALSE(sa.FromString(longHost.c_str()));
	const std::string longPort = "2001:db8::5|" + std::string(100, '7');
	(void) sa.FromString(longPort.c_str());
}

TEST(SystemAddressParse, ScopedLinkLocalRoundTrips)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("fe80::1%1"));
	EXPECT_EQ(std::string(sa.ToString(false)).rfind("fe80::1", 0), 0u);
}

TEST(SystemAddressLoopback, Ipv6LoopbackDetected)
{
	const SystemAddress lo("::1", 1);
	EXPECT_TRUE(lo.IsLoopback());

	SystemAddress sa;
	sa.SetToLoopback(6);
	EXPECT_STREQ(sa.ToString(false), "::1");
}

TEST(SystemAddressMapped, UnmapConvertsToIpv4)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("::ffff:192.0.2.7|99"));
	EXPECT_TRUE(sa.IsIPv4Mapped());
	sa.UnmapIPv4();
	EXPECT_EQ(sa.GetIPVersion(), 4);
	EXPECT_TRUE(sa == SystemAddress("192.0.2.7", 99));
	EXPECT_FALSE(sa.IsIPv4Mapped());
}

TEST(SystemAddressMapped, UnmapIsNoOpForNonMapped)
{
	SystemAddress v6("2001:db8::1", 5);
	const SystemAddress before = v6;
	v6.UnmapIPv4();
	EXPECT_TRUE(v6 == before);
	EXPECT_FALSE(v6.IsIPv4Mapped());

	SystemAddress v4("192.0.2.7", 5);
	EXPECT_FALSE(v4.IsIPv4Mapped());
	v4.UnmapIPv4();
	EXPECT_EQ(v4.GetIPVersion(), 4);
}

TEST(SystemAddressHash, SameAddressSameHash)
{
	const SystemAddress a4("192.0.2.1", 10), b4("192.0.2.1", 10);
	const SystemAddress a6("2001:db8::1", 10), b6("2001:db8::1", 10);
	EXPECT_EQ(SystemAddress::ToInteger(a4), SystemAddress::ToInteger(b4));
	EXPECT_EQ(SystemAddress::ToInteger(a6), SystemAddress::ToInteger(b6));
}
