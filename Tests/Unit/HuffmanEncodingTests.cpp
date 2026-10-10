/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <stdio.h>
#include <string.h>
#include <string>

#include "mafianet/string_compressor.h"
#include "mafianet/bit_stream.h"

using namespace MafiaNet;

/*
Description:
Golden-output tests for the Huffman tree behind StringCompressor, written when its node list moved
from DataStructures::LinkedList to std::list (std migration stage 1).

Why golden bytes rather than a round-trip: the tree is built by an insertion sort over node weights,
and the ORDER equal-weight nodes take decides the tree's shape, which decides the bit pattern each
character encodes to. A replacement container that sorted stably the other way would still build a
valid tree, still round-trip perfectly in-process, and still pass any encode/decode test -- while
silently changing what goes on the wire, so a peer on the old build could no longer read a string
from a peer on the new one.

The expectations below were captured from the pre-migration implementation and must not be edited to
match new output: if they fail, the encoding changed and the change is a wire break.

Success conditions: encoded bits and bytes match the captured values exactly, and strings round-trip.
*/

namespace
{
	// StringCompressor::Instance() dereferences a null singleton until someone takes a reference --
	// normally the RakPeer constructor, and there is no peer in a hermetic unit test.
	class HuffmanEncoding : public ::testing::Test
	{
	public:
		void SetUp() override { StringCompressor::AddReference(); }
		void TearDown() override { StringCompressor::RemoveReference(); }

		static std::string HexOf(const MafiaNet::BitStream &bs)
		{
			std::string out;
			const unsigned char *d = bs.GetData();
			char buf[4];
			for (unsigned i = 0; i < bs.GetNumberOfBytesUsed(); ++i)
			{
				snprintf(buf, sizeof(buf), "%02x", d[i]);
				out += buf;
			}
			return out;
		}

		void ExpectEncoding(const char *input, unsigned expectedBits, const char *expectedHex)
		{
			MafiaNet::BitStream bs;
			StringCompressor::Instance()->EncodeString(input, 512, &bs);
			EXPECT_EQ((unsigned)bs.GetNumberOfBitsUsed(), expectedBits) << "bit length changed for \"" << input << "\"";
			EXPECT_EQ(HexOf(bs), std::string(expectedHex)) << "encoded bytes changed for \"" << input << "\"";
		}
	};
} // namespace

TEST_F(HuffmanEncoding, ShortAsciiEncodesToTheCapturedBytes)
{
	ExpectEncoding("hello world", 89, "0000001c604a51e98cc9ca80");
}

TEST_F(HuffmanEncoding, MixedCaseEncodesToTheCapturedBytes)
{
	ExpectEncoding("MafiaNet", 81, "000000183eec7231885a80");
}

TEST_F(HuffmanEncoding, JsonPayloadEncodesToTheCapturedBytes)
{
	ExpectEncoding("{\"season\":\"winter\",\"map_file\":\"downtown.m2map\"}", 345,
		"0000009c3e523e7b0db827ccc1f3a50ac65f3d6f9e8ca55b139241f3307cdd1495c524f68ade8ca3e6f94e80");
}

// Runs of one character exercise the equal-weight ties most heavily, so this is the case a
// tie-breaking change is most likely to move.
TEST_F(HuffmanEncoding, RepeatedRunsEncodeToTheCapturedBytes)
{
	ExpectEncoding("aaaaaaaaaabbbbbbbbbbcccccccccc", 193,
		"0000005033333333331e79e79e79e79e7d1451451451451400");
}

TEST_F(HuffmanEncoding, StringsRoundTripThroughTheCompressor)
{
	const char *samples[] = {
		"hello world",
		"MafiaNet",
		"",
		"a",
		"{\"build\":\"m2o|1.2.3\"}"
	};
	for (int i = 0; i < 5; ++i)
	{
		MafiaNet::BitStream bs;
		StringCompressor::Instance()->EncodeString(samples[i], 512, &bs);
		bs.SetReadOffset(0);

		char decoded[512];
		memset(decoded, 0, sizeof(decoded));
		ASSERT_TRUE(StringCompressor::Instance()->DecodeString(decoded, 512, &bs))
			<< "decode failed for \"" << samples[i] << "\"";
		EXPECT_STREQ(decoded, samples[i]);
	}
}
