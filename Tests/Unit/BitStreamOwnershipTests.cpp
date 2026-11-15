// Owning-buffer APIs of BitStream and DataCompressor hand out
// std::unique_ptr<unsigned char[]> instead of a raw pointer the caller frees.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "mafianet/bit_stream.h"
#include "mafianet/data_compressor.h"

using namespace MafiaNet;

TEST(BitStreamOwnership, CopyDataReturnsIndependentCopy)
{
	BitStream bs;
	const unsigned char bytes[3] = {0x11, 0x22, 0x33};
	bs.Write((const char *) bytes, 3);

	std::unique_ptr<unsigned char[]> out;
	EXPECT_EQ(24u, bs.CopyData(out));
	ASSERT_NE(nullptr, out.get());
	EXPECT_EQ(0x11, out[0]);
	EXPECT_EQ(0x22, out[1]);
	EXPECT_EQ(0x33, out[2]);

	// Mutating the stream afterwards must not touch the copy.
	bs.Reset();
	bs.Write((const char *) "\xAA\xBB\xCC", 3);
	EXPECT_EQ(0x11, out[0]);
	EXPECT_EQ(0x22, out[1]);
	EXPECT_EQ(0x33, out[2]);
}

TEST(BitStreamOwnership, CopyDataOfEmptyStreamYieldsNull)
{
	BitStream bs;
	std::unique_ptr<unsigned char[]> out(new unsigned char[4]);
	EXPECT_EQ(0u, bs.CopyData(out));
	EXPECT_EQ(nullptr, out.get());
}

TEST(BitStreamOwnership, ReadAlignedBytesSafeAllocFillsUniquePtr)
{
	BitStream bs;
	const unsigned char bytes[4] = {1, 2, 3, 4};
	bs.WriteCompressed((unsigned int) 4);
	bs.AlignWriteToByteBoundary();
	bs.WriteAlignedBytes(bytes, 4);

	std::unique_ptr<unsigned char[]> out;
	unsigned int length = 0;
	ASSERT_TRUE(bs.ReadAlignedBytesSafeAlloc(out, length, 4096));
	EXPECT_EQ(4u, length);
	ASSERT_NE(nullptr, out.get());
	EXPECT_EQ(4, out[3]);
}

TEST(BitStreamOwnership, ReadAlignedBytesSafeAllocShortReadLeavesNull)
{
	BitStream bs;
	bs.WriteCompressed((unsigned int) 100);
	bs.Write((uint32_t) 0xDEADBEEF);

	std::unique_ptr<unsigned char[]> out(new unsigned char[1]);
	unsigned int length = 0;
	EXPECT_FALSE(bs.ReadAlignedBytesSafeAlloc(out, length, 4096));
	EXPECT_EQ(nullptr, out.get());
}

TEST(BitStreamOwnership, ReadAlignedBytesSafeAllocReplacesPreviousBuffer)
{
	// Successful read: the old buffer is replaced.
	{
		BitStream bs;
		const unsigned char bytes[2] = {7, 8};
		bs.WriteCompressed((unsigned int) 2);
		bs.AlignWriteToByteBoundary();
		bs.WriteAlignedBytes(bytes, 2);

		std::unique_ptr<unsigned char[]> out(new unsigned char[16]);
		unsigned int length = 0;
		ASSERT_TRUE(bs.ReadAlignedBytesSafeAlloc(out, length, 4096));
		EXPECT_EQ(2u, length);
		ASSERT_NE(nullptr, out.get());
		EXPECT_EQ(7, out[0]);
		EXPECT_EQ(8, out[1]);
	}
	// Failed read: the old buffer is released, not left behind.
	{
		BitStream bs;
		std::unique_ptr<unsigned char[]> out(new unsigned char[16]);
		unsigned int length = 0;
		EXPECT_FALSE(bs.ReadAlignedBytesSafeAlloc(out, length, 4096));
		EXPECT_EQ(nullptr, out.get());
	}
}
