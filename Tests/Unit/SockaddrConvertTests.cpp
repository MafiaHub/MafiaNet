/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hermetic tests for the sockaddr <-> SystemAddress conversions at the socket
 *  boundary (sockaddr_convert.h). A dual-stack socket (AF_INET6, V6ONLY=0)
 *  reports IPv4 peers as ::ffff:a.b.c.d and must be addressed that way; the
 *  rest of the library only ever sees the canonical AF_INET form.
 */

#include <gtest/gtest.h>

#include "mafianet/sockaddr_convert.h"
#include "mafianet/types.h"

#ifdef _WIN32
#include <ws2tcpip.h> // inet_pton, htons
#else
#include <arpa/inet.h> // inet_pton, htons
#endif
#include <cstring>
#include <string>

using namespace MafiaNet;

namespace
{
sockaddr_storage MakeV6(const char *ip, unsigned short port)
{
	sockaddr_storage ss;
	memset(&ss, 0, sizeof(ss));
	sockaddr_in6 *in6 = reinterpret_cast<sockaddr_in6 *>(&ss);
	in6->sin6_family = AF_INET6;
	in6->sin6_port = htons(port);
	inet_pton(AF_INET6, ip, &in6->sin6_addr);
	return ss;
}

SystemAddress V4Address(const char *ip, unsigned short port)
{
	SystemAddress a;
	EXPECT_TRUE(a.FromStringExplicitPort(ip, port, 4));
	return a;
}
}

TEST(SockaddrConvert, MappedSourceBecomesIpv4)
{
	SystemAddress out;
	ASSERT_TRUE(SockaddrToSystemAddress(MakeV6("::ffff:192.0.2.7", 4660), &out));
	EXPECT_EQ(out.GetIPVersion(), 4);
	EXPECT_FALSE(out.IsIPv4Mapped());
	EXPECT_EQ(std::string(out.ToString(false)), "192.0.2.7");
	EXPECT_EQ(out.GetPort(), 4660);
	EXPECT_EQ(out.debugPort, 4660);
	EXPECT_EQ(out, V4Address("192.0.2.7", 4660));
}

TEST(SockaddrConvert, PlainIpv6SourceStaysIpv6)
{
	SystemAddress out;
	ASSERT_TRUE(SockaddrToSystemAddress(MakeV6("2001:db8::7", 4660), &out));
	EXPECT_EQ(out.GetIPVersion(), 6);
	EXPECT_EQ(out.GetPort(), 4660);
}

TEST(SockaddrConvert, Ipv4TargetMappedOnDualStack)
{
	const SystemAddress target = V4Address("192.0.2.7", 4660);
	sockaddr_storage out;
	const socklen_t len = SystemAddressToSockaddr(target, true, &out, AF_INET6);
	ASSERT_EQ(len, (socklen_t) sizeof(sockaddr_in6));
	ASSERT_EQ(out.ss_family, AF_INET6);
	const sockaddr_in6 *in6 = reinterpret_cast<const sockaddr_in6 *>(&out);
	const unsigned char expected[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 7};
	EXPECT_EQ(memcmp(&in6->sin6_addr, expected, sizeof(expected)), 0);
	EXPECT_EQ(ntohs(in6->sin6_port), 4660);
}

TEST(SockaddrConvert, Ipv4TargetPlainOnV4Socket)
{
	const SystemAddress target = V4Address("192.0.2.7", 4660);
	sockaddr_storage out;
	const socklen_t len = SystemAddressToSockaddr(target, false, &out, AF_INET);
	ASSERT_EQ(len, (socklen_t) sizeof(sockaddr_in));
	ASSERT_EQ(out.ss_family, AF_INET);
	const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(&out);
	EXPECT_EQ(in->sin_addr.s_addr, target.address.addr4.sin_addr.s_addr);
	EXPECT_EQ(ntohs(in->sin_port), 4660);
}

TEST(SockaddrConvert, Ipv6TargetOnV4SocketRejected)
{
	SystemAddress target;
	ASSERT_TRUE(target.FromStringExplicitPort("2001:db8::7", 4660, 6));
	sockaddr_storage out;
	EXPECT_EQ(SystemAddressToSockaddr(target, false, &out, AF_INET), (socklen_t) 0);
}

TEST(SockaddrConvert, Ipv4TargetOnV6OnlySocketRejected)
{
	sockaddr_storage out;
	EXPECT_EQ(SystemAddressToSockaddr(V4Address("192.0.2.7", 4660), false, &out, AF_INET6), (socklen_t) 0);
}

TEST(SockaddrConvert, Ipv6TargetOnDualStackPassesThrough)
{
	SystemAddress target;
	ASSERT_TRUE(target.FromStringExplicitPort("2001:db8::7", 4660, 6));
	sockaddr_storage out;
	ASSERT_EQ(SystemAddressToSockaddr(target, true, &out, AF_INET6), (socklen_t) sizeof(sockaddr_in6));
	EXPECT_EQ(memcmp(&out, &target.address.addr6, sizeof(sockaddr_in6)), 0);
}

TEST(SockaddrConvert, UnknownSocketFamilyOnlyAppliesMapping)
{
	// AF_UNSPEC: the caller does not know the socket's family, so nothing is
	// rejected -- the address is passed through as-is.
	sockaddr_storage out;
	EXPECT_EQ(SystemAddressToSockaddr(V4Address("192.0.2.7", 4660), false, &out, AF_UNSPEC), (socklen_t) sizeof(sockaddr_in));
}

TEST(SockaddrConvert, UndecodableTargetFamilyRejected)
{
	SystemAddress target;
	memset(&target.address, 0, sizeof(target.address)); // sin_family 0
	sockaddr_storage out;
	EXPECT_EQ(SystemAddressToSockaddr(target, true, &out, AF_INET6), (socklen_t) 0);
}

TEST(SockaddrConvert, RoundTripThroughDualStackIsCanonical)
{
	// What the socket sends to and what it hands back on receive must compare
	// equal, or replies would not match the remote-system list.
	const SystemAddress target = V4Address("198.51.100.20", 61000);
	sockaddr_storage wire;
	ASSERT_NE(SystemAddressToSockaddr(target, true, &wire, AF_INET6), (socklen_t) 0);
	SystemAddress back;
	ASSERT_TRUE(SockaddrToSystemAddress(wire, &back));
	EXPECT_EQ(back, target);
	EXPECT_EQ(back.GetIPVersion(), 4);
}

// Which socket option sets the TTL / hop limit of a datagram (RakPeer::SendTTL,
// NAT punchthrough) depends on the socket and the target, not on the platform:
// the platform code only tries the candidates in order.

namespace
{
bool IsIpTtl(const HopLimitOption &option)
{
	return option.level == IPPROTO_IP && option.name == IP_TTL;
}
bool IsUnicastHops(const HopLimitOption &option)
{
	return option.level == IPPROTO_IPV6 && option.name == IPV6_UNICAST_HOPS;
}
}

TEST(HopLimitOptions, Ipv4SocketUsesIpTtl)
{
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	ASSERT_EQ(HopLimitOptionsFor(AF_INET, false, AF_INET, options), 1);
	EXPECT_TRUE(IsIpTtl(options[0]));
}

TEST(HopLimitOptions, Ipv6OnlySocketUsesUnicastHops)
{
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	ASSERT_EQ(HopLimitOptionsFor(AF_INET6, false, AF_INET6, options), 1);
	EXPECT_TRUE(IsUnicastHops(options[0]));
}

TEST(HopLimitOptions, DualStackIpv6TargetUsesUnicastHops)
{
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	ASSERT_EQ(HopLimitOptionsFor(AF_INET6, true, AF_INET6, options), 1);
	EXPECT_TRUE(IsUnicastHops(options[0]));
}

TEST(HopLimitOptions, DualStackIpv4TargetTriesIpTtlThenUnicastHops)
{
	// Linux and Windows apply IP_TTL to the IPv4 traffic of an AF_INET6 socket;
	// macOS rejects IP_TTL there (EINVAL) and applies the IPv6 hop
	// limit to IPv4-mapped datagrams instead.
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	ASSERT_EQ(HopLimitOptionsFor(AF_INET6, true, AF_INET, options), 2);
	EXPECT_TRUE(IsIpTtl(options[0]));
	EXPECT_TRUE(IsUnicastHops(options[1]));
}

TEST(HopLimitOptions, DualStackFamilyArgumentDoesNotMatter)
{
	// The socket's family argument is irrelevant once it is known to be
	// dual-stack (callers may pass the reported IPv4 bound address family).
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	ASSERT_EQ(HopLimitOptionsFor(AF_INET, true, AF_INET, options), 2);
	EXPECT_TRUE(IsIpTtl(options[0]));
	EXPECT_TRUE(IsUnicastHops(options[1]));
}

TEST(HopLimitOptions, UnreachableCombinationsHaveNoOption)
{
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	EXPECT_EQ(HopLimitOptionsFor(AF_INET, false, AF_INET6, options), 0);
	EXPECT_EQ(HopLimitOptionsFor(AF_INET6, false, AF_INET, options), 0);
	EXPECT_EQ(HopLimitOptionsFor(AF_INET6, true, AF_UNSPEC, options), 0);
	EXPECT_EQ(HopLimitOptionsFor(AF_INET, false, AF_UNSPEC, options), 0);
}

TEST(HopLimitOptions, UnknownSocketFamilyFollowsTarget)
{
	// AF_UNSPEC: the socket's family is not known, so the target's decides.
	HopLimitOption options[MAX_HOP_LIMIT_OPTIONS];
	ASSERT_EQ(HopLimitOptionsFor(AF_UNSPEC, false, AF_INET, options), 1);
	EXPECT_TRUE(IsIpTtl(options[0]));
	ASSERT_EQ(HopLimitOptionsFor(AF_UNSPEC, false, AF_INET6, options), 1);
	EXPECT_TRUE(IsUnicastHops(options[0]));
}

TEST(HopLimitRestore, SavedValueIsRestoredAsIs)
{
	EXPECT_EQ(HopLimitToRestore(64, false, 0), 64);
	EXPECT_EQ(HopLimitToRestore(64, true, 0), 64);
	EXPECT_EQ(HopLimitToRestore(128, true, 64), 128);
}

TEST(HopLimitRestore, KernelDefaultOnIpv6OnlySocketIsRestoredAsIs)
{
	// -1 ("use the kernel default") is what macOS reports for an
	// IPV6_UNICAST_HOPS that was never set; on an IPv6-only socket putting it
	// back is exact.
	EXPECT_EQ(HopLimitToRestore(-1, false, 64), -1);
}

TEST(HopLimitRestore, KernelDefaultOnDualStackSocketBecomesIpv4Default)
{
	// On macOS, explicitly setting IPV6_UNICAST_HOPS to -1 on a dual-stack
	// socket sends IPv4-mapped datagrams with TTL 255 afterwards, not with
	// the system default it had before (measured on macOS 26). Restore the
	// IPv4 default instead.
	EXPECT_EQ(HopLimitToRestore(-1, true, 64), 64);
	EXPECT_EQ(HopLimitToRestore(-1, true, 128), 128);
}

TEST(HopLimitRestore, UnknownIpv4DefaultFallsBackTo64)
{
	EXPECT_EQ(HopLimitToRestore(-1, true, 0), 64);
	EXPECT_EQ(HopLimitToRestore(-1, true, -1), 64);
}
