/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  HuffmanEncodingTree decodes bits from remote peers (StringCompressor,
 *  DataCompressor). The wire format is pinned by StringCompressorGoldenTests.cpp;
 *  these cases pin how decoding treats untrusted input and output bounds.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/huffman_encoding_tree.h"

#include <cstring>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_copy_constructible_v<HuffmanEncodingTree>, "HuffmanEncodingTree owns its nodes");
static_assert(!std::is_copy_assignable_v<HuffmanEncodingTree>, "HuffmanEncodingTree owns its nodes");
// A count that can exceed INT_MAX must stay unsigned all the way to the caller.
static_assert(std::is_same_v<size_t, decltype(std::declval<HuffmanEncodingTree &>().DecodeArray(std::declval<BitStream *>(), BitSize_t(), size_t(), std::declval<unsigned char *>()))>,
              "DecodeArray returns an unsigned size_t count");

namespace
{
void BuildUniformTree(HuffmanEncodingTree &tree, unsigned int frequency)
{
	unsigned int freq[256];
	for (int i = 0; i < 256; i++)
		freq[i] = frequency;
	tree.GenerateFromFrequencyTable(freq);
}

std::vector<unsigned char> AllBytes()
{
	std::vector<unsigned char> bytes(256);
	for (int i = 0; i < 256; i++)
		bytes[i] = (unsigned char) i;
	return bytes;
}
} // namespace

TEST(HuffmanEncodingTree, UngeneratedTreeRejectsBitsInsteadOfCrashing)
{
	HuffmanEncodingTree tree;
	unsigned char payload[4] = {0xA5, 0x5A, 0xFF, 0x00};

	BitStream in(payload, sizeof(payload), false);
	unsigned char out[8] = {};
	EXPECT_EQ(HuffmanEncodingTree::DECODE_ERROR, tree.DecodeArray(&in, 32, sizeof(out), out));

	BitStream decoded;
	EXPECT_FALSE(tree.DecodeArray(payload, 32, &decoded));
	EXPECT_EQ(0u, decoded.GetNumberOfBitsUsed());
}

TEST(HuffmanEncodingTree, ZeroBitsDecodeToNothingEvenWithoutATree)
{
	HuffmanEncodingTree tree;
	BitStream in;
	unsigned char out[1] = {0x7F};
	EXPECT_EQ(0u, tree.DecodeArray(&in, 0, sizeof(out), out));
	EXPECT_EQ(0x7F, out[0]);
}

TEST(HuffmanEncodingTree, FreedTreeRejectsBits)
{
	HuffmanEncodingTree tree;
	BuildUniformTree(tree, 1);
	tree.FreeMemory();

	unsigned char payload[2] = {0x12, 0x34};
	BitStream in(payload, sizeof(payload), false);
	unsigned char out[4];
	EXPECT_EQ(HuffmanEncodingTree::DECODE_ERROR, tree.DecodeArray(&in, 16, sizeof(out), out));
}

TEST(HuffmanEncodingTree, DecodeCountsCharactersPastMaxButNeverWritesThem)
{
	HuffmanEncodingTree tree;
	BuildUniformTree(tree, 1);
	std::vector<unsigned char> input = AllBytes();
	BitStream encoded;
	tree.EncodeArray(input.data(), input.size(), &encoded);

	// Room for 10 characters, followed by canaries that must survive.
	unsigned char out[16];
	memset(out, 0xCC, sizeof(out));
	const size_t count = tree.DecodeArray(&encoded, encoded.GetNumberOfBitsUsed(), 10, out);

	EXPECT_EQ(input.size(), count);
	EXPECT_EQ(0, memcmp(input.data(), out, 10));
	for (size_t i = 10; i < sizeof(out); i++)
		EXPECT_EQ(0xCC, out[i]) << "byte " << i << " written past maxCharsToWrite";
	// Every bit was consumed even though the output was full.
	EXPECT_EQ(0u, encoded.GetNumberOfUnreadBits());
}

TEST(HuffmanEncodingTree, UniformFrequenciesGiveEightBitCodes)
{
	HuffmanEncodingTree tree;
	BuildUniformTree(tree, 1);
	std::vector<unsigned char> input = AllBytes();
	BitStream encoded;
	tree.EncodeArray(input.data(), input.size(), &encoded);
	EXPECT_EQ(256u * 8u, encoded.GetNumberOfBitsUsed());
}

// Frequencies come from the network in DataCompressor. Summed as 32-bit values, 256 copies of
// 0xFFFFFFFF wrap, every merged node sorts below the leaves, and the tree degenerates into a
// chain with codes hundreds of bits long. With a 64-bit sum the tree stays balanced.
TEST(HuffmanEncodingTree, MaximalFrequenciesDoNotWrapTheWeightSum)
{
	HuffmanEncodingTree tree;
	BuildUniformTree(tree, 0xFFFFFFFFu);
	std::vector<unsigned char> input = AllBytes();
	BitStream encoded;
	tree.EncodeArray(input.data(), input.size(), &encoded);
	EXPECT_EQ(256u * 8u, encoded.GetNumberOfBitsUsed());

	std::vector<unsigned char> decoded(input.size());
	EXPECT_EQ(input.size(), tree.DecodeArray(&encoded, encoded.GetNumberOfBitsUsed(), decoded.size(), decoded.data()));
	EXPECT_EQ(input, decoded);
}

TEST(HuffmanEncodingTree, BitStreamOverloadRoundTrips)
{
	HuffmanEncodingTree tree;
	unsigned int freq[256];
	for (int i = 0; i < 256; i++)
		freq[i] = (unsigned int) (i % 7 + 1);
	tree.GenerateFromFrequencyTable(freq);

	const char text[] = "Huffman round trip through the BitStream overload";
	BitStream encoded;
	tree.EncodeArray((unsigned char *) text, sizeof(text) - 1, &encoded);

	// The padding is a strict prefix of a longer code, so it never completes a character.
	BitStream decoded;
	ASSERT_TRUE(tree.DecodeArray(encoded.GetData(), encoded.GetNumberOfBitsUsed(), &decoded));
	ASSERT_EQ((BitSize_t) (sizeof(text) - 1), decoded.GetNumberOfBytesUsed());
	EXPECT_EQ(0, memcmp(text, decoded.GetData(), sizeof(text) - 1));
}

TEST(HuffmanEncodingTree, RegeneratingReplacesTheTree)
{
	HuffmanEncodingTree tree;
	BuildUniformTree(tree, 0xFFFFFFFFu);
	unsigned int skewed[256];
	for (int i = 0; i < 256; i++)
		skewed[i] = 1;
	skewed['a'] = 1000000;
	tree.GenerateFromFrequencyTable(skewed);

	unsigned char a = 'a';
	BitStream encoded;
	tree.EncodeArray(&a, 1, &encoded);
	// 'a' dominates: one bit, padded to a byte.
	EXPECT_EQ(8u, encoded.GetNumberOfBitsUsed());
	unsigned char out[4];
	EXPECT_EQ(1u, tree.DecodeArray(&encoded, 1, sizeof(out), out));
	EXPECT_EQ('a', out[0]);
}
