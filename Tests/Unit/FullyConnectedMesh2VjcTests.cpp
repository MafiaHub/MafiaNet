/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  FullyConnectedMesh2 verified-join-capable message parsing.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/fully_connected_mesh2.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
void WriteEntry(BitStream &bs, uint64_t guid, const char *address)
{
	SystemAddress sa;
	EXPECT_TRUE(sa.FromString(address));
	bs.Write(RakNetGUID(guid));
	bs.Write(sa);
	bs.Write((unsigned char) 1);
}
}

TEST(FullyConnectedMesh2VJC, SkipToUserDataWithMixedFamilies)
{
	BitStream bs;
	bs.Write((MessageID) ID_FCM2_VERIFIED_JOIN_CAPABLE);
	bs.Write((unsigned short) 2);
	WriteEntry(bs, 11, "10.1.2.3|1000");
	WriteEntry(bs, 22, "2001:db8::2|2000");
	bs.Write((uint32_t) 0xC0FFEE);

	FullyConnectedMesh2::SkipToVJCUserData(&bs);
	uint32_t marker = 0;
	ASSERT_TRUE(bs.Read(marker));
	EXPECT_EQ(0xC0FFEEu, marker);
}

TEST(FullyConnectedMesh2VJC, SkipToUserDataStopsOnTruncatedList)
{
	BitStream bs;
	bs.Write((MessageID) ID_FCM2_VERIFIED_JOIN_CAPABLE);
	bs.Write((unsigned short) 3);
	WriteEntry(bs, 11, "10.1.2.3|1000");
	// Two entries are missing: must return, not assert or loop on garbage.
	FullyConnectedMesh2::SkipToVJCUserData(&bs);
	EXPECT_EQ(0u, bs.GetNumberOfUnreadBits());
}
