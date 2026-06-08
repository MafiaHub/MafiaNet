/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "PeerGuidTest.h"

#include "mafianet/types.h"
#include "mafianet/BitStream.h"

#include <type_traits>
#include <string.h>

// The type-safety guarantee: wire-identical to uint64_t, but not interchangeable with NetworkID.
static_assert(sizeof(PeerGuid) == 8,
	"PeerGuid must be 8 bytes to stay wire-compatible with RakNetGUID::g");
static_assert(std::is_trivially_copyable<PeerGuid>::value,
	"PeerGuid must be trivially copyable to serialize byte-identically");
static_assert(!std::is_convertible<PeerGuid, NetworkID>::value,
	"PeerGuid must NOT implicitly convert to NetworkID");
static_assert(!std::is_convertible<NetworkID, PeerGuid>::value,
	"NetworkID must NOT implicitly convert to PeerGuid");

int PeerGuidTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	const uint64_t samples[] = { 0u, 1u, 0x123456789ABCDEF0ull, (uint64_t)-1 };

	// ToPeerGuid()/ToGuid() round-trip the raw value both ways.
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
	{
		const uint64_t v = samples[i];

		const PeerGuid p = ToPeerGuid(RakNetGUID(v));
		if (static_cast<uint64_t>(p) != v)
		{
			if (isVerbose) printf("ToPeerGuid lost value 0x%llx -> 0x%llx\n",
				(unsigned long long) v, (unsigned long long) static_cast<uint64_t>(p));
			return 1;
		}

		const RakNetGUID back = ToGuid(p);
		if (back.g != v)
		{
			if (isVerbose) printf("ToGuid lost value 0x%llx -> 0x%llx\n",
				(unsigned long long) v, (unsigned long long) back.g);
			return 2;
		}
	}

	// UNASSIGNED_PEER_GUID mirrors UNASSIGNED_RAKNET_GUID across the converters.
	if (ToPeerGuid(UNASSIGNED_RAKNET_GUID) != UNASSIGNED_PEER_GUID)
	{
		if (isVerbose) printf("ToPeerGuid(UNASSIGNED_RAKNET_GUID) != UNASSIGNED_PEER_GUID\n");
		return 3;
	}
	if (ToGuid(UNASSIGNED_PEER_GUID).g != UNASSIGNED_RAKNET_GUID.g)
	{
		if (isVerbose) printf("ToGuid(UNASSIGNED_PEER_GUID) != UNASSIGNED_RAKNET_GUID\n");
		return 4;
	}

	// A PeerGuid serializes byte-identically to the uint64_t it replaces.
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
	{
		const uint64_t v = samples[i];
		const PeerGuid p = static_cast<PeerGuid>(v);

		BitStream bsPeer;
		bsPeer.Write(p);
		BitStream bsRaw;
		bsRaw.Write(v);

		if (bsPeer.GetNumberOfBitsUsed() != bsRaw.GetNumberOfBitsUsed() ||
			bsPeer.GetNumberOfBytesUsed() != bsRaw.GetNumberOfBytesUsed() ||
			memcmp(bsPeer.GetData(), bsRaw.GetData(), bsPeer.GetNumberOfBytesUsed()) != 0)
		{
			if (isVerbose) printf("PeerGuid wire bytes differ from uint64_t for 0x%llx\n",
				(unsigned long long) v);
			return 5;
		}

		BitStream bsIn(bsPeer.GetData(), bsPeer.GetNumberOfBytesUsed(), false);
		PeerGuid readBack = static_cast<PeerGuid>(0);
		if (!bsIn.Read(readBack) || readBack != p)
		{
			if (isVerbose) printf("PeerGuid did not round-trip through BitStream for 0x%llx\n",
				(unsigned long long) v);
			return 6;
		}
	}

	printf("PeerGuid is distinct from NetworkID and wire-compatible with uint64_t\n");
	return 0;
}

PeerGuidTest::PeerGuidTest(void)
{
}

PeerGuidTest::~PeerGuidTest(void)
{
}

RakString PeerGuidTest::GetTestName(void)
{
	return "PeerGuidTest";
}

RakString PeerGuidTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "ToPeerGuid did not preserve the raw 64-bit value";
	case 2:  return "ToGuid did not preserve the raw 64-bit value";
	case 3:  return "ToPeerGuid(UNASSIGNED_RAKNET_GUID) != UNASSIGNED_PEER_GUID";
	case 4:  return "ToGuid(UNASSIGNED_PEER_GUID) != UNASSIGNED_RAKNET_GUID";
	case 5:  return "PeerGuid serialized to different bytes than the raw uint64_t";
	case 6:  return "PeerGuid did not round-trip through a BitStream";
	default: return "Undefined Error";
	}
}

void PeerGuidTest::DestroyPeers(void)
{
}
