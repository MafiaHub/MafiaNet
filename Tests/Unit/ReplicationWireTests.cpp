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
	ASSERT_TRUE(window.Read(in, Kind::SnapshotAck));
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
		return window.Read(in, Kind::SnapshotAck);
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

TEST(ReplicationWire, SnapshotContextWidthsMatchTheWireAndRejectTruncatedHeaders)
{
	using namespace ReplicationWire;
	for (const auto kind : {Kind::Snapshot, Kind::SnapshotBatch, Kind::SnapshotAck, Kind::SnapshotSingleAck})
		for (const auto context : std::array<uint64_t, 5>{1, 127, 128, 16384, UINT64_MAX})
		{
			BitStream wire;
			WriteHeader(wire, kind, 41, 73, context);
			ASSERT_EQ(wire.GetNumberOfBytesUsed(), WireHeaderBytes(kind, context));
			const auto full = wire.GetNumberOfBitsUsed();
			for (BitSize_t length = 0; length <= full; ++length)
			{
				wire.SetWriteOffset(length);
				wire.ResetReadPointer();
				Header header{};
				ASSERT_EQ(ReadHeader(wire, header), length == full);
				if (length == full)
				{
					EXPECT_EQ(header.kind, kind);
					EXPECT_EQ(header.snapshotContext, context);
					EXPECT_EQ(header.clientToken, 0u);
					EXPECT_EQ(header.serverToken, 0u);
				}
			}
		}
}

TEST(ReplicationWire, OnlySnapshotTrafficCanUseANonzeroContext)
{
	using namespace ReplicationWire;
	auto recognized = [](Kind kind, uint64_t context)
	{
		BitStream wire;
		wire.Write(static_cast<MessageID>(ID_REPLICATION));
		wire.Write(static_cast<uint8_t>(SnapshotTag | static_cast<uint8_t>(kind)));
		WriteVar(wire, context);
		Header header{};
		return ReadHeader(wire, header);
	};
	EXPECT_FALSE(recognized(Kind::Accept, 1));
	EXPECT_FALSE(recognized(Kind::State, 1));
	EXPECT_FALSE(recognized(Kind::SnapshotAck, 0));
}

TEST(ReplicationWire, SingleReceiptAndTruncatedMaskHaveDistinctEncodings)
{
	using namespace ReplicationWire;
	BitStream in;
	WriteVar(in, UINT64_MAX);
	SnapshotAckWindow window;
	EXPECT_FALSE(window.Read(in, Kind::SnapshotAck));
	in.ResetReadPointer();
	ASSERT_TRUE(window.Read(in, Kind::SnapshotSingleAck));
	EXPECT_EQ(window.first, UINT64_MAX);
	EXPECT_EQ(window.mask, 1u);
	WriteVar(in, 1);
	in.ResetReadPointer();
	EXPECT_FALSE(window.Read(in, Kind::SnapshotSingleAck));
}

TEST(ReplicationWire, DeclineRoundTripsAndRejectsEveryTruncationAndBadFlag)
{
	using namespace ReplicationWire;
	for (bool declined : {false, true})
	{
		BitStream wire;
		Decline{5, declined}.Write(wire);
		const auto full = wire.GetNumberOfBitsUsed();
		EXPECT_EQ(full, (sizeof(NetworkID) + 1) * 8);
		// One spare byte, so trailing data can be offered too.
		wire.Write(static_cast<uint8_t>(0));
		for (BitSize_t length = 0; length <= full + 8; ++length)
		{
			BitStream in(wire.GetData(), wire.GetNumberOfBytesUsed(), false);
			in.SetWriteOffset(length);
			Decline read;
			ASSERT_EQ(read.Read(in), length == full) << length;
			if (length == full)
			{
				EXPECT_EQ(read.id, 5u);
				EXPECT_EQ(read.declined, declined);
			}
		}
	}
	BitStream badFlag;
	badFlag.Write(NetworkID{5});
	badFlag.Write(static_cast<uint8_t>(2));
	Decline read;
	EXPECT_FALSE(read.Read(badFlag));
	BitStream unassigned;
	Decline{UNASSIGNED_NETWORK_ID, true}.Write(unassigned);
	EXPECT_FALSE(read.Read(unassigned));
}
