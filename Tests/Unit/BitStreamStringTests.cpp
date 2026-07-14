/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/BitStream.h"

#include <cstring>
#include <string>

using namespace MafiaNet;

// Regression tests for the std::string serialization trap: the catch-all
// BitStream::Write/Read templates used to raw-copy a std::string's internal
// representation (pointer/size/capacity) instead of its characters, producing
// aliased heap pointers and eventual double-frees. std::string now has a
// dedicated specialization that uses the same length-prefixed wire format as
// RakString. These tests verify the round-trip and wire-compatibility.

namespace
{
	const std::string kOriginal = "Hello, MafiaNet!";
}

// On-wire size must be the length-prefixed string form (unsigned short
// length + the characters), NOT sizeof(std::string). This is the check
// that fails when the catch-all template raw-copies the object.
TEST(BitStreamString, WritesLengthPrefixedWireFormat)
{
	BitStream bsOut;
	bsOut.Write(kOriginal);

	const BitSize_t expectedBytes = (BitSize_t)(sizeof(unsigned short) + kOriginal.length());
	EXPECT_EQ(bsOut.GetNumberOfBytesUsed(), expectedBytes)
		<< "std::string serialized as a raw object copy (wrong byte count)";
}

// Round-trip a std::string through a BitStream.
TEST(BitStreamString, RoundTripsThroughBitStream)
{
	BitStream bsOut;
	bsOut.Write(kOriginal);

	BitStream bsIn(bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), false);
	std::string roundTripped;
	ASSERT_TRUE(bsIn.Read(roundTripped)) << "Failed to read std::string back from the bitstream";
	EXPECT_EQ(roundTripped, kOriginal)
		<< "std::string round-trip produced a different value: got '" << roundTripped << "'";
}

// std::string written -> RakString read.
TEST(BitStreamString, StdStringToRakStringInterop)
{
	BitStream bs;
	bs.Write(kOriginal);
	RakString rs;
	ASSERT_TRUE(bs.Read(rs)) << "std::string -> RakString wire interop failed (read)";
	EXPECT_EQ(strcmp(rs.C_String(), kOriginal.c_str()), 0)
		<< "std::string -> RakString wire interop failed: '" << rs.C_String() << "'";
}

// RakString written -> std::string read.
TEST(BitStreamString, RakStringToStdStringInterop)
{
	RakString rs(kOriginal.c_str());
	BitStream bs;
	bs.Write(rs);
	std::string out;
	ASSERT_TRUE(bs.Read(out)) << "RakString -> std::string wire interop failed (read)";
	EXPECT_EQ(out, kOriginal)
		<< "RakString -> std::string wire interop failed: '" << out << "'";
}

// Binary safety: a string with an embedded null must round-trip in full.
TEST(BitStreamString, EmbeddedNullRoundTrips)
{
	std::string binary("a\0b\0c", 5);
	BitStream bs;
	bs.Write(binary);
	std::string out;
	ASSERT_TRUE(bs.Read(out)) << "std::string with embedded null did not round-trip (read)";
	EXPECT_EQ(out, binary)
		<< "std::string with embedded null did not round-trip (len " << (unsigned) out.size() << ")";
}
