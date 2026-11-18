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
#include <memory>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h> // inet_pton, htons
#else
#include <arpa/inet.h> // inet_pton, htons
#endif

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

TEST(SystemAddressParse, OverlongStringDoesNotOverflow)
{
	SystemAddress sa;
	// Longer than any host name: rejected before it reaches the resolver.
	const std::string longHost(300, 'a');
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

TEST(SystemAddressParse, MappedLiteralIsCanonicalIpv4)
{
	// A typed ::ffff:a.b.c.d is the IPv4 host: replies from it arrive as plain
	// AF_INET, so the parsed address must compare equal to that.
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("::ffff:203.0.113.5|60000"));
	EXPECT_EQ(sa.GetIPVersion(), 4);
	EXPECT_FALSE(sa.IsIPv4Mapped());
	EXPECT_TRUE(sa == SystemAddress("203.0.113.5", 60000));

	SystemAddress explicitPort;
	ASSERT_TRUE(explicitPort.FromStringExplicitPort("::ffff:203.0.113.5", 60000, 6));
	EXPECT_EQ(explicitPort.GetIPVersion(), 4);
	EXPECT_TRUE(explicitPort == SystemAddress("203.0.113.5", 60000));
}

TEST(SystemAddressMapped, UnmapConvertsToIpv4)
{
	// FromString canonicalizes mapped literals, so build the mapped form directly.
	SystemAddress sa;
	sockaddr_in6 mapped;
	memset(&mapped, 0, sizeof(mapped));
	mapped.sin6_family = AF_INET6;
	mapped.sin6_port = htons(99);
	ASSERT_EQ(inet_pton(AF_INET6, "::ffff:192.0.2.7", &mapped.sin6_addr), 1);
	memcpy(&sa.address.addr6, &mapped, sizeof(mapped));
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

// --- ToString buffer bounds ---

namespace
{
// The longest IPv6 text without a scope id (39 characters) and the longest port.
const char *const LONG_IPV6 = "2001:db8:1234:5678:9abc:def0:1234:5678";
}

// The longest ToString() result: a 45-character IPv6 text ("ffff:...:255.255.255.255"), '%' and a 10-digit
// scope id, the port delineator, a 5-digit port and the terminator. INET6_ADDRSTRLEN is not used: it is 46 on
// POSIX but 65 on Windows.
constexpr size_t LONGEST_TO_STRING = 45 + 1 + 10 + 1 + 5 + 1;
static_assert(SystemAddress::TO_STRING_BUFFER_SIZE >= LONGEST_TO_STRING, "ToString's documented minimum must hold any address, scope id and port");

TEST(SystemAddressToString, SizedOverloadNeverOverflowsASmallBuffer)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromStringExplicitPort(LONG_IPV6, 65535));

	// Exactly 32 bytes on the heap, so AddressSanitizer reports any write past it.
	std::unique_ptr<char[]> tinyBuffer(new char[32]);
	memset(tinyBuffer.get(), 'X', 32);
	sa.ToString(true, tinyBuffer.get(), static_cast<size_t>(32));
	EXPECT_LT(strnlen(tinyBuffer.get(), 32), 32u) << "result is not NUL-terminated within the buffer";
	EXPECT_STREQ(tinyBuffer.get(), "") << "a result that does not fit is empty, never a truncated address";
}

TEST(SystemAddressToString, SizedOverloadWritesTheWholeAddressWhenItFits)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromStringExplicitPort(LONG_IPV6, 65535));

	char buf[64];
	sa.ToString(true, buf, sizeof(buf));
	EXPECT_STREQ(buf, "2001:db8:1234:5678:9abc:def0:1234:5678|65535");
	sa.ToString(false, buf, sizeof(buf));
	EXPECT_STREQ(buf, LONG_IPV6);
}

TEST(SystemAddressToString, ExplicitColonPreservesLegacyIpv4PortSpelling)
{
	const SystemAddress address("192.0.2.1", 65535);
	char text[sizeof("192.0.2.1:65535")];
	address.ToString(true, text, sizeof(text), ':');
	EXPECT_STREQ("192.0.2.1:65535", text);
	address.ToString(true, text, sizeof(text) - 1, ':');
	EXPECT_STREQ("", text);
}

TEST(SystemAddressToString, ExactFitIsWrittenOneByteLessIsEmpty)
{
	const SystemAddress sa("192.0.2.1", 7);
	const std::string expected = "192.0.2.1|7";
	std::unique_ptr<char[]> exact(new char[expected.size() + 1]);
	sa.ToString(true, exact.get(), expected.size() + 1);
	EXPECT_EQ(std::string(exact.get()), expected);

	std::unique_ptr<char[]> shortBuf(new char[expected.size()]);
	sa.ToString(true, shortBuf.get(), expected.size());
	EXPECT_STREQ(shortBuf.get(), "");
}

TEST(SystemAddressToString, UnassignedIntoASmallBufferDoesNotOverflow)
{
	std::unique_ptr<char[]> tinyBuffer(new char[8]);
	UNASSIGNED_SYSTEM_ADDRESS.ToString(true, tinyBuffer.get(), static_cast<size_t>(8));
	EXPECT_STREQ(tinyBuffer.get(), "");

	char buf[SystemAddress::TO_STRING_BUFFER_SIZE];
	UNASSIGNED_SYSTEM_ADDRESS.ToString(true, buf, sizeof(buf));
	EXPECT_STREQ(buf, "UNASSIGNED_SYSTEM_ADDRESS");
}

TEST(SystemAddressToString, UnsizedOverloadFitsTheDocumentedMinimum)
{
	// The longest possible result: 39-character address, a 10-digit scope id and a 5-digit port.
	SystemAddress sa;
	// A global address with a scope id set by hand: link-local ones carry the scope in the address bytes on macOS.
	ASSERT_TRUE(sa.FromStringExplicitPort(LONG_IPV6, 65535));
	sa.address.addr6.sin6_scope_id = 4294967295u;

	std::unique_ptr<char[]> exact(new char[SystemAddress::TO_STRING_BUFFER_SIZE]);
	sa.ToString(true, exact.get());
	EXPECT_STREQ(exact.get(), "2001:db8:1234:5678:9abc:def0:1234:5678%4294967295|65535");
}

TEST(SystemAddressToString, ScopeIdIsPrintedNumerically)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromStringExplicitPort("fe80::1", 5));
	sa.address.addr6.sin6_scope_id = 7;
	EXPECT_STREQ(sa.ToString(true), "fe80::1%7|5");

	sa.address.addr6.sin6_scope_id = 0;
	EXPECT_STREQ(sa.ToString(true), "fe80::1|5");
}

// --- Host/port splitting (FromString) ---

TEST(SplitHostAndPort, LongHostNameIsNotTruncated)
{
	// Longer than INET6_ADDRSTRLEN, like a cloud provider's public DNS name.
	const std::string name = "ec2-203-0-113-25.eu-west-3.compute.example-cloud-provider.com";
	ASSERT_GT(name.size(), 55u);
	char host[256];
	char port[32];
	ASSERT_TRUE(SplitHostAndPort((name + "|60000").c_str(), '|', host, sizeof(host), port, sizeof(port)));
	EXPECT_EQ(std::string(host), name);
	EXPECT_STREQ(port, "60000");

	ASSERT_TRUE(SplitHostAndPort(name.c_str(), '|', host, sizeof(host), port, sizeof(port)));
	EXPECT_EQ(std::string(host), name);
	EXPECT_STREQ(port, "");
}

TEST(SplitHostAndPort, HostOfMaximumLengthFitsLongerIsRejected)
{
	char host[256];
	char port[32];
	const std::string longest(255, 'h');
	ASSERT_TRUE(SplitHostAndPort(longest.c_str(), '|', host, sizeof(host), port, sizeof(port)));
	EXPECT_EQ(std::string(host), longest);

	const std::string tooLong(256, 'h');
	EXPECT_FALSE(SplitHostAndPort(tooLong.c_str(), '|', host, sizeof(host), port, sizeof(port)));
	EXPECT_FALSE(SplitHostAndPort((tooLong + "|1").c_str(), '|', host, sizeof(host), port, sizeof(port)));
}

TEST(SplitHostAndPort, OverlongPortIsRejected)
{
	char host[256];
	char port[8];
	EXPECT_FALSE(SplitHostAndPort("example.com|123456789", '|', host, sizeof(host), port, sizeof(port)));
	ASSERT_TRUE(SplitHostAndPort("example.com|1234567", '|', host, sizeof(host), port, sizeof(port)));
	EXPECT_STREQ(port, "1234567");
}

TEST(SplitHostAndPort, NulDelimiterMeansNoPort)
{
	char host[256];
	char port[32];
	ASSERT_TRUE(SplitHostAndPort("2001:db8::1", '\0', host, sizeof(host), port, sizeof(port)));
	EXPECT_STREQ(host, "2001:db8::1");
	EXPECT_STREQ(port, "");
}

TEST(SystemAddressParse, HostNameLongerThan255IsRejected)
{
	SystemAddress sa;
	const std::string name = std::string(250, 'a') + ".example";
	EXPECT_FALSE(sa.FromString(name.c_str()));
	EXPECT_FALSE(sa.FromStringExplicitPort(name.c_str(), 1));
}

// A loopback literal in the other family stands for the loopback of the requested one, with or without a port.
TEST(SystemAddressParse, Ipv4LoopbackAsIpv6IsIpv6Loopback)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("127.0.0.1", '|', 6));
	EXPECT_EQ(sa.GetIPVersion(), 6);
	char buf[SystemAddress::TO_STRING_BUFFER_SIZE];
	sa.ToString(false, buf, sizeof(buf));
	EXPECT_STREQ(buf, "::1");
}

TEST(SystemAddressParse, Ipv4LoopbackWithPortAsIpv6IsIpv6LoopbackWithPort)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("127.0.0.1|5", '|', 6));
	EXPECT_EQ(sa.GetIPVersion(), 6);
	EXPECT_EQ(sa.GetPort(), 5);
	char buf[SystemAddress::TO_STRING_BUFFER_SIZE];
	sa.ToString(false, buf, sizeof(buf));
	EXPECT_STREQ(buf, "::1");
}

TEST(SystemAddressParse, Ipv6LoopbackAsIpv4IsIpv4Loopback)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("::1", '|', 4));
	EXPECT_EQ(sa.GetIPVersion(), 4);
	char buf[SystemAddress::TO_STRING_BUFFER_SIZE];
	sa.ToString(false, buf, sizeof(buf));
	EXPECT_STREQ(buf, "127.0.0.1");
}

TEST(SystemAddressParse, Ipv6LoopbackWithPortAsIpv4IsIpv4LoopbackWithPort)
{
	SystemAddress sa;
	ASSERT_TRUE(sa.FromString("::1|7", '|', 4));
	EXPECT_EQ(sa.GetIPVersion(), 4);
	EXPECT_EQ(sa.GetPort(), 7);
	char buf[SystemAddress::TO_STRING_BUFFER_SIZE];
	sa.ToString(false, buf, sizeof(buf));
	EXPECT_STREQ(buf, "127.0.0.1");
}

TEST(SystemAddressParse, LoopbackExplicitPortIsConvertedToRequestedFamily)
{
	SystemAddress v6;
	ASSERT_TRUE(v6.FromStringExplicitPort("127.0.0.1", 9, 6));
	EXPECT_EQ(v6.GetIPVersion(), 6);
	EXPECT_EQ(v6.GetPort(), 9);
	SystemAddress v4;
	ASSERT_TRUE(v4.FromStringExplicitPort("::1", 9, 4));
	EXPECT_EQ(v4.GetIPVersion(), 4);
	EXPECT_EQ(v4.GetPort(), 9);
}
