/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/variable_delta_serializer.h"

using namespace MafiaNet;

namespace
{
const RakNetGUID kRemote(1234);

// One acked serialize of a single int, with a fresh context as Replica3::Serialize uses; returns the
// number of bits written (0 when nothing changed).
BitSize_t SerializeAcked(VariableDeltaSerializer &vds, uint32_t receipt, int value)
{
	VariableDeltaSerializer::SerializationContext context;
	BitStream bs;
	vds.BeginUnreliableAckedSerialize(&context, kRemote, &bs, receipt);
	vds.SerializeVariable(&context, value);
	vds.EndSerialize(&context);
	return bs.GetNumberOfBitsUsed();
}
}

TEST(VariableDeltaSerializer, AckedSerializeWritesOnlyChangedValues)
{
	VariableDeltaSerializer vds;
	EXPECT_GT(SerializeAcked(vds, 1, 5), 0u);
	EXPECT_EQ(SerializeAcked(vds, 2, 5), 0u);
	EXPECT_GT(SerializeAcked(vds, 3, 6), 0u);
}

TEST(VariableDeltaSerializer, LostReceiptMakesTheValueResend)
{
	VariableDeltaSerializer vds;
	ASSERT_GT(SerializeAcked(vds, 1, 5), 0u);
	vds.OnMessageReceipt(kRemote, 1, false);
	EXPECT_GT(SerializeAcked(vds, 2, 5), 0u);
	// The resend was recorded under receipt 2; acking it leaves nothing dirty.
	vds.OnMessageReceipt(kRemote, 2, true);
	EXPECT_EQ(SerializeAcked(vds, 3, 5), 0u);
}

TEST(VariableDeltaSerializer, AckedReceiptDoesNotResend)
{
	VariableDeltaSerializer vds;
	ASSERT_GT(SerializeAcked(vds, 1, 5), 0u);
	vds.OnMessageReceipt(kRemote, 1, true);
	// A loss reported after the ack finds no history for that receipt.
	vds.OnMessageReceipt(kRemote, 1, false);
	EXPECT_EQ(SerializeAcked(vds, 2, 5), 0u);
}

TEST(VariableDeltaSerializer, UnknownReceiptOrSystemIsIgnored)
{
	VariableDeltaSerializer vds;
	vds.OnMessageReceipt(kRemote, 7, false);
	ASSERT_GT(SerializeAcked(vds, 1, 5), 0u);
	vds.OnMessageReceipt(kRemote, 99, false);
	vds.OnMessageReceipt(RakNetGUID(5678), 1, false);
	EXPECT_EQ(SerializeAcked(vds, 2, 5), 0u);
}

TEST(VariableDeltaSerializer, RemovedSystemStartsOverWithFullState)
{
	VariableDeltaSerializer vds;
	ASSERT_GT(SerializeAcked(vds, 1, 5), 0u);
	ASSERT_GT(SerializeAcked(vds, 2, 6), 0u);
	// Frees the history of receipts 1 and 2, which were never acked.
	vds.RemoveRemoteSystemVariableHistory(kRemote);
	EXPECT_GT(SerializeAcked(vds, 3, 6), 0u);
}

TEST(VariableDeltaSerializer, DeserializeReadsWhatAckedSerializeWrote)
{
	VariableDeltaSerializer vds;
	VariableDeltaSerializer::SerializationContext context;
	BitStream bs;
	vds.BeginUnreliableAckedSerialize(&context, kRemote, &bs, 1);
	vds.SerializeVariable(&context, 42);
	vds.EndSerialize(&context);

	VariableDeltaSerializer::DeserializationContext readContext;
	vds.BeginDeserialize(&readContext, &bs);
	int value = 0;
	EXPECT_TRUE(vds.DeserializeVariable(&readContext, value));
	vds.EndDeserialize(&readContext);
	EXPECT_EQ(value, 42);
}
