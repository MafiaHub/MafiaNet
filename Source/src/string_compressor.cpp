/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
///


#include "mafianet/string_compressor.h"
#include <cstdlib>
#include <memory>
#include <utility>
#include "mafianet/huffman_encoding_tree.h"
#include "mafianet/bit_stream.h"
#include <algorithm>
#include <stdexcept>
#include "mafianet/assert.h"
#include <string.h>

#include <memory.h>


using namespace MafiaNet;

StringCompressor *StringCompressor::instance = 0;
int StringCompressor::referenceCount = 0;

void StringCompressor::AddReference(void)
{
	if (++referenceCount == 1)
	{
		instance = new StringCompressor;
	}
}
void StringCompressor::RemoveReference(void)
{
	RakAssert(referenceCount > 0);

	if (referenceCount > 0)
	{
		if (--referenceCount == 0)
		{
			delete instance;
			instance = 0;
		}
	}
}

StringCompressor *StringCompressor::Instance(void)
{
	return instance;
}

unsigned int englishCharacterFrequencies[256] =
    {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        722,
        0,
        0,
        2,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        11084,
        58,
        63,
        1,
        0,
        31,
        0,
        317,
        64,
        64,
        44,
        0,
        695,
        62,
        980,
        266,
        69,
        67,
        56,
        7,
        73,
        3,
        14,
        2,
        69,
        1,
        167,
        9,
        1,
        2,
        25,
        94,
        0,
        195,
        139,
        34,
        96,
        48,
        103,
        56,
        125,
        653,
        21,
        5,
        23,
        64,
        85,
        44,
        34,
        7,
        92,
        76,
        147,
        12,
        14,
        57,
        15,
        39,
        15,
        1,
        1,
        1,
        2,
        3,
        0,
        3611,
        845,
        1077,
        1884,
        5870,
        841,
        1057,
        2501,
        3212,
        164,
        531,
        2019,
        1330,
        3056,
        4037,
        848,
        47,
        2586,
        2919,
        4771,
        1707,
        535,
        1106,
        152,
        1243,
        100,
        0,
        2,
        0,
        10,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0};

StringCompressor::StringCompressor()
{
	// Make a default tree immediately, since this is used for RPC possibly from multiple threads at the same time
	std::unique_ptr<HuffmanEncodingTree> huffmanEncodingTree(new HuffmanEncodingTree);
	huffmanEncodingTree->GenerateFromFrequencyTable(englishCharacterFrequencies);

	huffmanEncodingTrees[0] = std::move(huffmanEncodingTree);
}
void StringCompressor::GenerateTreeFromStrings(unsigned char *input, unsigned inputLength, uint8_t languageId)
{
	unsigned index;
	unsigned int frequencyTable[256];

	// Nothing to build a tree from: keep the language's current tree, if any.
	if (inputLength == 0)
		return;

	// Zero out the frequency table
	memset(frequencyTable, 0, sizeof(frequencyTable));

	// Generate the frequency table from the strings
	for (index = 0; index < inputLength; index++)
		frequencyTable[input[index]]++;

	// Build the tree, replacing (and freeing) the language's previous one
	std::unique_ptr<HuffmanEncodingTree> huffmanEncodingTree(new HuffmanEncodingTree);
	huffmanEncodingTree->GenerateFromFrequencyTable(frequencyTable);
	huffmanEncodingTrees[languageId] = std::move(huffmanEncodingTree);
}

StringCompressor::~StringCompressor() = default;

void StringCompressor::EncodeString(const char *input, int maxCharsToWrite, MafiaNet::BitStream *output, uint8_t languageId)
{
	EncodeString(std::string_view(input ? input : ""), maxCharsToWrite, output, languageId);
}

void StringCompressor::EncodeString(std::string_view input, int maxCharsToWrite, MafiaNet::BitStream *output, uint8_t languageId)
{
	const auto tree = huffmanEncodingTrees.find(languageId);
	if (tree == huffmanEncodingTrees.end())
		return;
	HuffmanEncodingTree *huffmanEncodingTree = tree->second.get();

	if (maxCharsToWrite > 0)
		input = input.substr(0, static_cast<size_t>(maxCharsToWrite - 1));
	if (input.size() > BitStream::MAX_STRING_BYTES)
		throw std::length_error("Compressed string exceeds maximum length");
	MafiaNet::BitStream encodedBitStream;
	huffmanEncodingTree->EncodeArray(reinterpret_cast<const unsigned char *>(input.data()), input.size(), &encodedBitStream);
	const uint32_t stringBitLength = encodedBitStream.GetNumberOfBitsUsed();

	output->WriteCompressed(stringBitLength);

	output->WriteBits(encodedBitStream.GetData(), stringBitLength);
}

bool StringCompressor::DecodeString(char *output, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId)
{
	// Always leave a valid empty string behind, whichever early return is taken below.
	if (maxCharsToWrite > 0)
		output[0] = 0;
	const auto tree = huffmanEncodingTrees.find(languageId);
	if (tree == huffmanEncodingTrees.end())
		return false;
	if (maxCharsToWrite <= 0)
		return false;
	HuffmanEncodingTree *huffmanEncodingTree = tree->second.get();

	uint32_t stringBitLength;

	if (input->ReadCompressed(stringBitLength) == false)
		return false;

	if ((unsigned) input->GetNumberOfUnreadBits() < stringBitLength)
		return false;

	// Unsigned all the way: the decoded count may exceed maxCharsToWrite (and INT_MAX), and must never index output.
	const size_t outputSize = (size_t) maxCharsToWrite;
	const size_t charsInStream = huffmanEncodingTree->DecodeArray(input, stringBitLength, outputSize, (unsigned char *) output);
	if (charsInStream == HuffmanEncodingTree::DECODE_ERROR)
	{
		output[0] = 0;
		return false;
	}

	if (charsInStream < outputSize)
		output[charsInStream] = 0;
	else
		output[outputSize - 1] = 0;

	return true;
}
bool StringCompressor::DecodeString(std::string *output, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId)
{
	output->clear();
	const auto tree = huffmanEncodingTrees.find(languageId);
	uint32_t bits;
	if (tree == huffmanEncodingTrees.end() || !input->ReadCompressed(bits) || bits > input->GetNumberOfUnreadBits())
		return false;
	const size_t limit = maxCharsToWrite > 0 ? std::min<size_t>(maxCharsToWrite - 1, BitStream::MAX_STRING_BYTES) : BitStream::MAX_STRING_BYTES;
	// A Huffman symbol consumes at least one bit.
	output->resize(std::min<size_t>(bits, limit));
	const size_t decoded = tree->second->DecodeArray(input, bits, output->size(), reinterpret_cast<unsigned char *>(output->data()));
	if (decoded == HuffmanEncodingTree::DECODE_ERROR || decoded > BitStream::MAX_STRING_BYTES)
	{
		output->clear();
		return false;
	}
	output->resize(std::min(decoded, output->size()));
	return true;
}
