/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <sodium.h>

#include <string.h>

#include "mafianet/handshake_challenge.h"

using namespace MafiaNet;

namespace
{

constexpr TimeUS kSecond = 1000000;
// Aligned to a bucket start: the window is 3 whole buckets, so how far into a bucket
// a challenge was issued decides whether 3.9 s still verifies.
constexpr TimeUS kStart = (TimeUS) 96 << 20;

SystemAddress Addr(const char *ip, unsigned short port)
{
	SystemAddress a;
	a.FromStringExplicitPort(ip, port);
	return a;
}

class HandshakeChallengeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_GE(sodium_init(), 0);
		const uint8_t secret[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
		challenge.SetSecretForTesting(secret);
	}

	HandshakeChallenge challenge;
};

} // namespace

TEST_F(HandshakeChallengeTest, AcceptsFreshChallenge)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const uint64_t c = challenge.Generate(kStart, from);
	EXPECT_TRUE(challenge.Verify(c, kStart, from));
	EXPECT_EQ((uint16_t) c, HandshakeChallenge::TimeBucket(kStart));
}

TEST_F(HandshakeChallengeTest, AcceptsWithinFourSeconds)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const uint64_t c = challenge.Generate(kStart, from);
	EXPECT_TRUE(challenge.Verify(c, kStart + 3900000, from));
}

TEST_F(HandshakeChallengeTest, RejectsAfterWindow)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const uint64_t c = challenge.Generate(kStart, from);
	EXPECT_FALSE(challenge.Verify(c, kStart + 5 * kSecond, from));
}

TEST_F(HandshakeChallengeTest, RejectsChallengeFromTheFuture)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const uint64_t c = challenge.Generate(kStart + 10 * kSecond, from);
	EXPECT_FALSE(challenge.Verify(c, kStart, from));
}

TEST_F(HandshakeChallengeTest, RejectsOtherPort)
{
	const uint64_t c = challenge.Generate(kStart, Addr("192.168.1.5", 4000));
	EXPECT_FALSE(challenge.Verify(c, kStart, Addr("192.168.1.5", 4001)));
}

TEST_F(HandshakeChallengeTest, RejectsOtherIp)
{
	const uint64_t c = challenge.Generate(kStart, Addr("192.168.1.5", 4000));
	EXPECT_FALSE(challenge.Verify(c, kStart, Addr("192.168.1.6", 4000)));
}

TEST_F(HandshakeChallengeTest, RejectsFlippedBit)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const uint64_t c = challenge.Generate(kStart, from);
	for (int bit = 0; bit < 64; bit++)
		EXPECT_FALSE(challenge.Verify(c ^ (1ull << bit), kStart, from)) << "bit " << bit;
}

TEST_F(HandshakeChallengeTest, DifferentSecretsDisagree)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const uint64_t c = challenge.Generate(kStart, from);

	HandshakeChallenge other;
	const uint8_t secret[16] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
	other.SetSecretForTesting(secret);
	EXPECT_FALSE(other.Verify(c, kStart, from));
	EXPECT_NE(other.Generate(kStart, from), c);
}

TEST_F(HandshakeChallengeTest, RandomSecretsDisagree)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	HandshakeChallenge a, b;
	EXPECT_NE(a.Generate(kStart, from), b.Generate(kStart, from));
}

TEST_F(HandshakeChallengeTest, Ipv4AndMappedIpv6AgreeOnlyWhenSameEndpoint)
{
	const SystemAddress v4 = Addr("192.168.1.5", 4000);

	// The same endpoint spelled as an IPv4-mapped IPv6 address (::ffff:192.168.1.5).
	SystemAddress mapped;
	memset(&mapped.address, 0, sizeof(mapped.address));
	mapped.address.addr6.sin6_family = AF_INET6;
	mapped.address.addr6.sin6_port = v4.GetPortNetworkOrder();
	mapped.address.addr6.sin6_addr.s6_addr[10] = 0xFF;
	mapped.address.addr6.sin6_addr.s6_addr[11] = 0xFF;
	memcpy(mapped.address.addr6.sin6_addr.s6_addr + 12, &v4.address.addr4.sin_addr.s_addr, 4);
	ASSERT_EQ(mapped.GetIPVersion(), 6);

	EXPECT_EQ(challenge.Generate(kStart, v4), challenge.Generate(kStart, mapped));
	EXPECT_TRUE(challenge.Verify(challenge.Generate(kStart, v4), kStart, mapped));

	SystemAddress otherPort = mapped;
	otherPort.address.addr6.sin6_port = Addr("192.168.1.5", 4001).GetPortNetworkOrder();
	EXPECT_NE(challenge.Generate(kStart, v4), challenge.Generate(kStart, otherPort));

	SystemAddress otherIp = mapped;
	otherIp.address.addr6.sin6_addr.s6_addr[15] ^= 1;
	EXPECT_NE(challenge.Generate(kStart, v4), challenge.Generate(kStart, otherIp));
}

TEST_F(HandshakeChallengeTest, NativeIpv6EndpointsAreDistinguished)
{
	const SystemAddress a = Addr("2001:db8::1", 4000);
	const SystemAddress b = Addr("2001:db8::2", 4000);
	ASSERT_EQ(a.GetIPVersion(), 6);
	const uint64_t c = challenge.Generate(kStart, a);
	EXPECT_TRUE(challenge.Verify(c, kStart, a));
	EXPECT_FALSE(challenge.Verify(c, kStart, b));
}

TEST_F(HandshakeChallengeTest, WrapsAroundTimeBucketOverflow)
{
	const SystemAddress from = Addr("192.168.1.5", 4000);
	const TimeUS issued = (TimeUS) 0xFFFF << 20;
	const TimeUS later = (TimeUS) 0x10001 << 20; // bucket wraps to 0x0001
	ASSERT_EQ(HandshakeChallenge::TimeBucket(issued), 0xFFFF);
	ASSERT_EQ(HandshakeChallenge::TimeBucket(later), 0x0001);
	const uint64_t c = challenge.Generate(issued, from);
	EXPECT_TRUE(challenge.Verify(c, later, from));
	EXPECT_FALSE(challenge.Verify(c, later + 5 * kSecond, from));
}
