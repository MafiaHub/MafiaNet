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

TEST(ReplicationWire, EmptySnapshotAckIsRejected)
{
	BitStream body;
	ReplicationWire::SnapshotAcks acks;
	EXPECT_FALSE(acks.Read(body));
}

TEST(ReplicationWire, SnapshotAckDecodesOneThroughTheMaximumRecordCount)
{
	using namespace ReplicationWire;
	BitStream body;
	for (unsigned n = 1; n <= MaxBatchRecords; ++n)
	{
		body.Write(static_cast<NetworkID>(n));
		body.Write(static_cast<uint64_t>(n * 7));
		body.ResetReadPointer();
		SnapshotAcks acks;
		ASSERT_TRUE(acks.Read(body));
		ASSERT_EQ(acks.count, n);
		EXPECT_EQ(acks.records[n - 1].id, n);
		EXPECT_EQ(acks.records[n - 1].seq, n * 7);
	}
}

TEST(ReplicationWire, TooManySnapshotAckRecordsRejectTheWholeReply)
{
	using namespace ReplicationWire;
	BitStream body;
	for (unsigned i = 0; i <= MaxBatchRecords; ++i)
	{
		body.Write(static_cast<NetworkID>(i));
		body.Write(uint64_t{1});
	}
	SnapshotAcks acks;
	EXPECT_FALSE(acks.Read(body));
	EXPECT_EQ(acks.count, 0u);
}

TEST(ReplicationWire, ZeroSequenceRejectsEarlierSnapshotAckRecords)
{
	BitStream body;
	body.Write(NetworkID{1});
	body.Write(uint64_t{2});
	body.Write(NetworkID{3});
	body.Write(uint64_t{0});
	ReplicationWire::SnapshotAcks acks;
	EXPECT_FALSE(acks.Read(body));
	EXPECT_EQ(acks.count, 0u);
}
