/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/handshake_challenge.h"

#include <sodium.h>

#include <string.h>

namespace MafiaNet
{

namespace
{

#pragma pack(push, 1)
struct ChallengeInput
{
	uint16_t time;
	uint16_t port;
	uint8_t ipv6[16];
};
#pragma pack(pop)

static_assert(sizeof(ChallengeInput) == 20, "challenge input must be packed with no padding");
static_assert(crypto_shorthash_KEYBYTES == 16, "secret size");
static_assert(crypto_shorthash_BYTES == 8, "hash size");

constexpr uint64_t kHashMask = 0xFFFFFFFFFFFF0000ull;

} // namespace

HandshakeChallenge::HandshakeChallenge()
{
	randombytes_buf(secret, sizeof(secret));
}

HandshakeChallenge::~HandshakeChallenge()
{
	sodium_memzero(secret, sizeof(secret));
}

void HandshakeChallenge::SetSecretForTesting(const uint8_t newSecret[16])
{
	memcpy(secret, newSecret, sizeof(secret));
}

uint64_t HandshakeChallenge::Hash(uint16_t timeBucket, const SystemAddress &from) const
{
	ChallengeInput in;
	memset(&in, 0, sizeof(in));
	in.time = timeBucket;
	in.port = from.GetPortNetworkOrder();
	if (from.GetIPVersion() == 4)
	{
		// IPv4-mapped IPv6: ::ffff:a.b.c.d
		in.ipv6[10] = 0xFF;
		in.ipv6[11] = 0xFF;
		memcpy(in.ipv6 + 12, &from.address.addr4.sin_addr.s_addr, 4);
	}
	else
	{
		memcpy(in.ipv6, &from.address.addr6.sin6_addr, 16);
	}

	uint8_t out[crypto_shorthash_BYTES];
	crypto_shorthash(out, (const unsigned char *) &in, sizeof(in), secret);
	uint64_t h = 0;
	for (int i = 0; i < 8; i++)
		h |= (uint64_t) out[i] << (8 * i);
	return h;
}

uint64_t HandshakeChallenge::Generate(TimeUS now, const SystemAddress &from) const
{
	const uint16_t time = TimeBucket(now);
	return (Hash(time, from) & kHashMask) | time;
}

bool HandshakeChallenge::Verify(uint64_t challenge, TimeUS now, const SystemAddress &from) const
{
	const uint16_t then = (uint16_t) challenge;
	const uint16_t elapsed = (uint16_t) (TimeBucket(now) - then);
	if (elapsed > TimeBucket(4000000))
		return false;

	const uint64_t expected = (Hash(then, from) & kHashMask) | then;
	uint8_t a[8], b[8];
	for (int i = 0; i < 8; i++)
	{
		a[i] = (uint8_t) (expected >> (8 * i));
		b[i] = (uint8_t) (challenge >> (8 * i));
	}
	return sodium_memcmp(a, b, sizeof(a)) == 0;
}

} // namespace MafiaNet
