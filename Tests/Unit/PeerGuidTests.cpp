/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/types.h"
#include "mafianet/BitStream.h"

#include <type_traits>
#include <string.h>

using namespace MafiaNet;

// Verifies the native strong-typed PeerGuid (issue #25): that it is distinct
// from NetworkID at compile time, round-trips through ToPeerGuid()/ToGuid(),
// and serializes byte-identically to the raw uint64_t it replaces.

// The type-safety guarantee: wire-identical to uint64_t, but not interchangeable with NetworkID.
static_assert(sizeof(PeerGuid) == 8,
	"PeerGuid must be 8 bytes to stay wire-compatible with RakNetGUID::g");
static_assert(std::is_trivially_copyable<PeerGuid>::value,
	"PeerGuid must be trivially copyable to serialize byte-identically");
static_assert(!std::is_convertible<PeerGuid, NetworkID>::value,
	"PeerGuid must NOT implicitly convert to NetworkID");
static_assert(!std::is_convertible<NetworkID, PeerGuid>::value,
	"NetworkID must NOT implicitly convert to PeerGuid");

namespace
{
	const uint64_t kSamples[] = { 0u, 1u, 0x123456789ABCDEF0ull, (uint64_t)-1 };
}

// ToPeerGuid()/ToGuid() round-trip the raw value both ways.
TEST(PeerGuid, ConvertersRoundTripRawValue)
{
	for (size_t i = 0; i < sizeof(kSamples) / sizeof(kSamples[0]); ++i)
	{
		const uint64_t v = kSamples[i];

		const PeerGuid p = ToPeerGuid(RakNetGUID(v));
		ASSERT_EQ(static_cast<uint64_t>(p), v) << "ToPeerGuid did not preserve the raw 64-bit value";

		const RakNetGUID back = ToGuid(p);
		ASSERT_EQ(back.g, v) << "ToGuid did not preserve the raw 64-bit value";
	}
}

// UNASSIGNED_PEER_GUID mirrors UNASSIGNED_RAKNET_GUID across the converters.
TEST(PeerGuid, UnassignedSentinelMirrorsAcrossConverters)
{
	ASSERT_TRUE(ToPeerGuid(UNASSIGNED_RAKNET_GUID) == UNASSIGNED_PEER_GUID) << "ToPeerGuid(UNASSIGNED_RAKNET_GUID) != UNASSIGNED_PEER_GUID";
	ASSERT_EQ(ToGuid(UNASSIGNED_PEER_GUID).g, UNASSIGNED_RAKNET_GUID.g) << "ToGuid(UNASSIGNED_PEER_GUID) != UNASSIGNED_RAKNET_GUID";
}

// A PeerGuid serializes byte-identically to the uint64_t it replaces.
TEST(PeerGuid, SerializesByteIdenticallyToUint64)
{
	for (size_t i = 0; i < sizeof(kSamples) / sizeof(kSamples[0]); ++i)
	{
		const uint64_t v = kSamples[i];
		const PeerGuid p = static_cast<PeerGuid>(v);

		BitStream bsPeer;
		bsPeer.Write(p);
		BitStream bsRaw;
		bsRaw.Write(v);

		ASSERT_TRUE(bsPeer.GetNumberOfBitsUsed() == bsRaw.GetNumberOfBitsUsed() &&
			bsPeer.GetNumberOfBytesUsed() == bsRaw.GetNumberOfBytesUsed() &&
			memcmp(bsPeer.GetData(), bsRaw.GetData(), bsPeer.GetNumberOfBytesUsed()) == 0)
			<< "PeerGuid serialized to different bytes than the raw uint64_t for 0x" << std::hex << v;

		BitStream bsIn(bsPeer.GetData(), bsPeer.GetNumberOfBytesUsed(), false);
		PeerGuid readBack = static_cast<PeerGuid>(0);
		ASSERT_TRUE(bsIn.Read(readBack) && readBack == p)
			<< "PeerGuid did not round-trip through a BitStream for 0x" << std::hex << v;
	}
}
