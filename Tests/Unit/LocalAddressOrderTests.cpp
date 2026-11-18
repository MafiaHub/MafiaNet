/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// SocketLayer::OrderInternalIds decides which local addresses a peer advertises
// as its internal IDs (MAXIMUM_NUMBER_OF_INTERNAL_IDS of them). NAT punchthrough
// relies on the LAN IPv4 addresses being among them, so IPv4 goes first, then
// global IPv6; link-local IPv6 (useless without a scope id), loopback and
// IPv4-mapped entries are dropped. Pure logic on caller-built addresses: hermetic.

#include "mafianet/guid_util.h"
#include <gtest/gtest.h>

#include "mafianet/socket_layer.h"
#include "mafianet/types.h"

#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h> // inet_pton, htons
#else
#include <arpa/inet.h> // inet_pton, htons
#endif

using namespace MafiaNet;

namespace
{
SystemAddress Addr(const char *ip, unsigned short port = 0)
{
	SystemAddress sa;
	EXPECT_TRUE(sa.FromString(ip)) << ip;
	sa.SetPortHostOrder(port);
	return sa;
}

// ::ffff:<ipv4>, built directly: FromString canonicalizes a mapped literal to AF_INET.
SystemAddress MappedAddress(const char *ipv4, unsigned short port)
{
	SystemAddress sa;
	sockaddr_in6 mapped;
	memset(&mapped, 0, sizeof(mapped));
	mapped.sin6_family = AF_INET6;
	mapped.sin6_port = htons(port);
	mapped.sin6_addr.s6_addr[10] = 0xff;
	mapped.sin6_addr.s6_addr[11] = 0xff;
	EXPECT_EQ(inet_pton(AF_INET, ipv4, &mapped.sin6_addr.s6_addr[12]), 1);
	memcpy(&sa.address.addr6, &mapped, sizeof(mapped));
	sa.debugPort = port;
	return sa;
}

std::string Ip(const SystemAddress &sa)
{
	return MafiaNet::to_string(sa, false);
}
}

TEST(LocalAddressOrder, Ipv4First)
{
	std::vector<SystemAddress> list = {Addr("2001:db8::1"), Addr("192.168.1.10"), Addr("2001:db8::2"), Addr("10.0.0.5")};

	const int n = SocketLayer::OrderInternalIds(list.data(), (int) list.size());

	ASSERT_EQ(n, 4);
	// Families are grouped, each group keeps the order it arrived in.
	EXPECT_EQ(Ip(list[0]), "192.168.1.10");
	EXPECT_EQ(Ip(list[1]), "10.0.0.5");
	EXPECT_EQ(Ip(list[2]), "2001:db8::1");
	EXPECT_EQ(Ip(list[3]), "2001:db8::2");
}

TEST(LocalAddressOrder, LinkLocalDropped)
{
	// fe80::/10 covers fe80:: through febf::; fec0:: is outside it.
	std::vector<SystemAddress> list = {Addr("fe80::1"), Addr("2001:db8::7"), Addr("febf::1"), Addr("10.1.2.3"), Addr("fe9a::5"), Addr("fec0::1")};

	const int n = SocketLayer::OrderInternalIds(list.data(), (int) list.size());

	ASSERT_EQ(n, 3);
	EXPECT_EQ(Ip(list[0]), "10.1.2.3");
	EXPECT_EQ(Ip(list[1]), "2001:db8::7");
	EXPECT_EQ(Ip(list[2]), "fec0::1");
	for (int i = n; i < (int) list.size(); ++i)
		EXPECT_EQ(list[i], UNASSIGNED_SYSTEM_ADDRESS) << "index " << i;
}

TEST(LocalAddressOrder, MappedAndUnassignedDropped)
{
	std::vector<SystemAddress> list = {MappedAddress("192.0.2.7", 0), UNASSIGNED_SYSTEM_ADDRESS, Addr("2001:db8::9"), Addr("172.16.0.1")};

	const int n = SocketLayer::OrderInternalIds(list.data(), (int) list.size());

	ASSERT_EQ(n, 2);
	EXPECT_EQ(Ip(list[0]), "172.16.0.1");
	EXPECT_EQ(Ip(list[1]), "2001:db8::9");
}

TEST(LocalAddressOrder, LoopbackDropped)
{
	// The hostname lookup path (GetMyIP on Windows) can return loopback; it must
	// never be advertised, nor picked by GetFirstBindableIP. All of 127.0.0.0/8
	// is loopback; ::1 is the only IPv6 one (::2 is not).
	std::vector<SystemAddress> list = {Addr("::1"), Addr("127.0.0.1"), Addr("192.168.0.4"), Addr("127.1.2.3"), Addr("::2"), Addr("2001:db8::4")};

	const int n = SocketLayer::OrderInternalIds(list.data(), (int) list.size());

	ASSERT_EQ(n, 3);
	EXPECT_EQ(Ip(list[0]), "192.168.0.4");
	EXPECT_EQ(Ip(list[1]), "::2");
	EXPECT_EQ(Ip(list[2]), "2001:db8::4");
	for (int i = n; i < (int) list.size(); ++i)
		EXPECT_EQ(list[i], UNASSIGNED_SYSTEM_ADDRESS) << "index " << i;
}

TEST(LocalAddressOrder, OnlyLoopbackLeavesNothing)
{
	std::vector<SystemAddress> list = {Addr("127.0.0.1"), Addr("::1")};

	EXPECT_EQ(SocketLayer::OrderInternalIds(list.data(), (int) list.size()), 0);
	EXPECT_EQ(list[0], UNASSIGNED_SYSTEM_ADDRESS);
	EXPECT_EQ(list[1], UNASSIGNED_SYSTEM_ADDRESS);
}

TEST(LocalAddressOrder, KeepsAtMostMaxInternalIds)
{
	// Global IPv6 addresses arriving first must not push out the IPv4 ones.
	static_assert(MAXIMUM_NUMBER_OF_INTERNAL_IDS == 10, "test inputs assume 10 internal IDs");
	std::vector<SystemAddress> list;
	for (int i = 1; i <= 8; ++i)
		list.push_back(Addr(("2001:db8::" + std::to_string(i)).c_str()));
	for (int i = 1; i <= 4; ++i)
		list.push_back(Addr(("10.0.0." + std::to_string(i)).c_str()));
	ASSERT_EQ(list.size(), 12u);

	const int n = SocketLayer::OrderInternalIds(list.data(), (int) list.size());

	ASSERT_EQ(n, MAXIMUM_NUMBER_OF_INTERNAL_IDS);
	for (int i = 0; i < 4; ++i)
		EXPECT_EQ(Ip(list[i]), "10.0.0." + std::to_string(i + 1)) << "index " << i;
	for (int i = 4; i < 10; ++i)
		EXPECT_EQ(Ip(list[i]), "2001:db8::" + std::to_string(i - 3)) << "index " << i;
	EXPECT_EQ(list[10], UNASSIGNED_SYSTEM_ADDRESS);
	EXPECT_EQ(list[11], UNASSIGNED_SYSTEM_ADDRESS);
}

TEST(LocalAddressOrder, EmptyInputReturnsZero)
{
	EXPECT_EQ(SocketLayer::OrderInternalIds(nullptr, 0), 0);
}
