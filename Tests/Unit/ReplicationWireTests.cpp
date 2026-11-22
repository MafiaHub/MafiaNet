// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "../../Source/src/replication_wire.h"

using namespace MafiaNet;

TEST(ReplicationWire, ReadsACompleteHeaderAndRejectsEachTruncatedPrefix)
{
	BitStream encoded;
	ReplicationWire::WriteHeader(encoded, ReplicationWire::Kind::Spawn, 41, 73);
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
