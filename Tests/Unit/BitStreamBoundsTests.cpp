/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Reads whose length comes from the data being read. Found by the
 *  bit_stream fuzz harness (Tests/Fuzz).
 */

#include <gtest/gtest.h>

#include <cstring>

#include "mafianet/bit_stream.h"

using namespace MafiaNet;

namespace
{
// 0x20000001 << 3 wraps to 8 in 32 bits: the old bounds checks compared
// against 8 bits, passed, and copied 512 MB.
const unsigned int kWrappingLength = 0x20000001u;

void FillSixteenBytes(BitStream &bs)
{
	for (int i = 0; i < 16; ++i)
		bs.Write((unsigned char) i);
}
}

TEST(BitStreamBounds, AlignedReadLongerThanTheStreamWhoseBitCountWrapsIsRejected)
{
	BitStream bs;
	FillSixteenBytes(bs);
	char out[16] = {};
	EXPECT_FALSE(bs.Read(out, kWrappingLength));
	EXPECT_EQ(0u, bs.GetReadOffset());
}

TEST(BitStreamBounds, UnalignedReadLongerThanTheStreamWhoseBitCountWrapsIsRejected)
{
	BitStream bs;
	FillSixteenBytes(bs);
	bs.IgnoreBits(1);
	char out[16] = {};
	EXPECT_FALSE(bs.Read(out, kWrappingLength));
	EXPECT_EQ(1u, bs.GetReadOffset());
}

TEST(BitStreamBounds, ReadAlignedBytesWhoseBitCountWrapsIsRejected)
{
	BitStream bs;
	FillSixteenBytes(bs);
	unsigned char out[16] = {};
	EXPECT_FALSE(bs.ReadAlignedBytes(out, kWrappingLength));
}

TEST(BitStreamBounds, ReadExactlyTheRemainingBytesSucceeds)
{
	BitStream bs;
	FillSixteenBytes(bs);
	char out[16] = {};
	ASSERT_TRUE(bs.Read(out, 16));
	EXPECT_EQ(15, out[15]);
	EXPECT_FALSE(bs.Read(out, 1));
}

TEST(BitStreamBounds, SafeAllocReadThatRunsShortReturnsNoBuffer)
{
	// Claims 100 bytes, carries 4: the buffer used to be returned allocated
	// with nothing read into it, leaking for callers that free on success.
	BitStream bs;
	bs.WriteCompressed((unsigned int) 100);
	bs.Write((uint32_t) 0xDEADBEEF);

	char *buffer = nullptr;
	unsigned int length = 0;
	EXPECT_FALSE(bs.ReadAlignedBytesSafeAlloc(&buffer, length, 4096));
	EXPECT_EQ(nullptr, buffer);
}

TEST(BitStreamBounds, ReadDeltaAtTheEndOfTheStreamFailsAndLeavesTheValue)
{
	// The "was written" flag was read into an uninitialized bool and then
	// branched on even when that read had failed.
	BitStream bs;
	uint32_t value = 1234;
	EXPECT_FALSE(bs.ReadDelta(value));
	EXPECT_EQ(1234u, value);
	EXPECT_FALSE(bs.ReadCompressedDelta(value));
	EXPECT_EQ(1234u, value);
}
