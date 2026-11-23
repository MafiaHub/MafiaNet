// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "../../Source/src/replication_wire.h"

using namespace MafiaNet;

TEST(ReplicationWire, ReadsACompleteHeaderAndRejectsEachTruncatedPrefix)
{
	BitStream encoded;
	ReplicationWire::WriteHeader(encoded, ReplicationWire::Kind::Spawn, 41, 73);
	EXPECT_EQ(encoded.GetNumberOfBytesUsed(), 19u);
	for (BitSize_t length = 0; length <= encoded.GetNumberOfBitsUsed(); ++length)
	{
		BitStream in(encoded.GetData(), encoded.GetNumberOfBytesUsed(), false);
		in.SetWriteOffset(length);
		ReplicationWire::Header header{};
		ASSERT_EQ(ReplicationWire::ReadHeader(in, header), length == encoded.GetNumberOfBitsUsed());
		if (length == encoded.GetNumberOfBitsUsed())
		{
			EXPECT_EQ(header.kind, ReplicationWire::Kind::Spawn);
			EXPECT_EQ(header.clientToken, 41u);
			EXPECT_EQ(header.serverToken, 73u);
		}
	}
}

TEST(ReplicationWire, BatchFramingChecksAllBoundariesBeforeDelivery)
{
	using namespace ReplicationWire;
	BitStream wire;
	WriteHeader(wire, Kind::StateBatch, 1, 2);
	const uint16_t bits = 193;
	unsigned char body[25]{};
	for (unsigned i = 0; i < 2; ++i)
	{
		wire.Write(bits);
		wire.WriteAlignedBytes(body, sizeof(body));
	}
	const auto full = wire.GetNumberOfBitsUsed();
	for (BitSize_t length = HeaderBytes * 8; length <= full; ++length)
	{
		BitStream input(wire.GetData(), wire.GetNumberOfBytesUsed(), false);
		input.SetWriteOffset(length);
		input.SetReadOffset(HeaderBytes * 8);
		StateBatchView view;
		EXPECT_EQ(view.Read(input), length == full);
	}
	for (unsigned i = 2; i <= MaxBatchRecords; ++i)
	{
		wire.Write(bits);
		wire.WriteAlignedBytes(body, sizeof(body));
	}
	wire.SetReadOffset(HeaderBytes * 8);
	StateBatchView view;
	EXPECT_FALSE(view.Read(wire));
}

TEST(ReplicationWire, SelectiveAckPreservesHolesAndTheHighestBit)
{
	using namespace ReplicationWire;
	BitStream in;
	WriteVar(in, UINT64_MAX - 31);
	WriteVar(in, 0x80000005u);
	SnapshotAckWindow window;
	ASSERT_TRUE(window.Read(in));
	EXPECT_EQ(window.first, UINT64_MAX - 31);
	EXPECT_EQ(window.mask, 0x80000005u);
}

TEST(ReplicationWire, InvalidWindowsCannotConfirmAnySequence)
{
	using namespace ReplicationWire;
	auto valid = [](uint64_t first, uint64_t mask)
	{
		BitStream in;
		WriteVar(in, first);
		WriteVar(in, mask);
		SnapshotAckWindow window;
		return window.Read(in);
	};
	EXPECT_FALSE(valid(0, 1));
	EXPECT_FALSE(valid(1, 0));
	EXPECT_FALSE(valid(1, 2));
	EXPECT_FALSE(valid(UINT64_MAX, 3));
	EXPECT_FALSE(valid(1, uint64_t{1} << 32 | 1));
	EXPECT_TRUE(valid(UINT64_MAX, 1));
}

TEST(ReplicationWire, CompactIntegersRejectTruncationOverflowAndOverlongEncodings)
{
	using namespace ReplicationWire;
	for (uint64_t value : std::array<uint64_t, 6>{0, 127, 128, 16384, UINT32_MAX, UINT64_MAX})
	{
		BitStream encoded;
		WriteVar(encoded, value);
		EXPECT_EQ(encoded.GetNumberOfBytesUsed(), VarBytes(value));
		const auto full = encoded.GetNumberOfBitsUsed();
		uint64_t decoded = 7;
		for (unsigned bits = 0; bits < full; ++bits)
		{
			encoded.SetWriteOffset(bits);
			encoded.ResetReadPointer();
			ASSERT_FALSE(ReadVar(encoded, decoded));
			ASSERT_EQ(decoded, 7u);
		}
		encoded.SetWriteOffset(full);
		encoded.ResetReadPointer();
		ASSERT_TRUE(ReadVar(encoded, decoded));
		EXPECT_EQ(decoded, value);
	}
	BitStream overlong;
	overlong.Write(uint8_t{0x81});
	overlong.Write(uint8_t{0});
	uint64_t ignored = 0;
	EXPECT_FALSE(ReadVar(overlong, ignored));
}

TEST(ReplicationWire, CompactBatchValidatesSequenceRangeAndEveryRecordBoundary)
{
	using namespace ReplicationWire;
	auto read = [](uint64_t base, BitSize_t cut)
	{
		BitStream wire;
		WriteVar(wire, base);
		WriteVar(wire, 128);
		for (unsigned i = 0; i < 2; ++i)
		{
			WriteVar(wire, 9);
			wire.Write(uint8_t{1});
			wire.Write(uint8_t{0x80});
		}
		wire.SetWriteOffset(wire.GetNumberOfBitsUsed() - cut);
		StateBatchView view;
		return view.Read(wire, true);
	};
	EXPECT_TRUE(read(UINT64_MAX - 1, 0));
	EXPECT_FALSE(read(UINT64_MAX, 0));
	EXPECT_FALSE(read(0, 0));
	for (unsigned cut = 1; cut <= 24; ++cut)
		EXPECT_FALSE(read(1, cut));
}
