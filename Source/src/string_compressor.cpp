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
#include "mafianet/huffman_encoding_tree.h"
#include "mafianet/bit_stream.h"
#include "mafianet/string.h"
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
	DataStructures::Map<int, HuffmanEncodingTree *>::IMPLEMENT_DEFAULT_COMPARISON();

	// Make a default tree immediately, since this is used for RPC possibly from multiple threads at the same time
	HuffmanEncodingTree *huffmanEncodingTree = new HuffmanEncodingTree;
	huffmanEncodingTree->GenerateFromFrequencyTable(englishCharacterFrequencies);

	huffmanEncodingTrees.Set(0, huffmanEncodingTree);
}
void StringCompressor::GenerateTreeFromStrings(unsigned char *input, unsigned inputLength, uint8_t languageId)
{
	HuffmanEncodingTree *huffmanEncodingTree;
	if (huffmanEncodingTrees.Has(languageId))
	{
		huffmanEncodingTree = huffmanEncodingTrees.Get(languageId);
		delete huffmanEncodingTree;
	}

	unsigned index;
	unsigned int frequencyTable[256];

	if (inputLength == 0)
		return;

	// Zero out the frequency table
	memset(frequencyTable, 0, sizeof(frequencyTable));

	// Generate the frequency table from the strings
	for (index = 0; index < inputLength; index++)
		frequencyTable[input[index]]++;

	// Build the tree
	huffmanEncodingTree = new HuffmanEncodingTree;
	huffmanEncodingTree->GenerateFromFrequencyTable(frequencyTable);
	huffmanEncodingTrees.Set(languageId, huffmanEncodingTree);
}

StringCompressor::~StringCompressor()
{
	for (unsigned i = 0; i < huffmanEncodingTrees.Size(); i++)
		delete huffmanEncodingTrees[i];
}

void StringCompressor::EncodeString(const char *input, int maxCharsToWrite, MafiaNet::BitStream *output, uint8_t languageId)
{
	HuffmanEncodingTree *huffmanEncodingTree;
	if (huffmanEncodingTrees.Has(languageId) == false)
		return;
	huffmanEncodingTree = huffmanEncodingTrees.Get(languageId);

	if (input == 0)
	{
		output->WriteCompressed((uint32_t) 0);
		return;
	}

	MafiaNet::BitStream encodedBitStream;

	uint32_t stringBitLength;

	int charsToWrite;

	if (maxCharsToWrite <= 0 || (int) strlen(input) < maxCharsToWrite)
		charsToWrite = (int) strlen(input);
	else
		charsToWrite = maxCharsToWrite - 1;

	huffmanEncodingTree->EncodeArray((unsigned char *) input, charsToWrite, &encodedBitStream);

	stringBitLength = (uint32_t) encodedBitStream.GetNumberOfBitsUsed();

	output->WriteCompressed(stringBitLength);

	output->WriteBits(encodedBitStream.GetData(), stringBitLength);
}

bool StringCompressor::DecodeString(char *output, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId)
{
	HuffmanEncodingTree *huffmanEncodingTree;
	// Always leave a valid empty string behind, whichever early return is taken below.
	if (maxCharsToWrite > 0)
		output[0] = 0;
	if (huffmanEncodingTrees.Has(languageId) == false)
		return false;
	if (maxCharsToWrite <= 0)
		return false;
	huffmanEncodingTree = huffmanEncodingTrees.Get(languageId);

	uint32_t stringBitLength;
	int bytesInStream;

	output[0] = 0;

	if (input->ReadCompressed(stringBitLength) == false)
		return false;

	if ((unsigned) input->GetNumberOfUnreadBits() < stringBitLength)
		return false;

	bytesInStream = huffmanEncodingTree->DecodeArray(input, stringBitLength, maxCharsToWrite, (unsigned char *) output);

	if (bytesInStream < maxCharsToWrite)
		output[bytesInStream] = 0;
	else
		output[maxCharsToWrite - 1] = 0;

	return true;
}
#ifdef _CSTRING_COMPRESSOR
void StringCompressor::EncodeString(const CString &input, int maxCharsToWrite, MafiaNet::BitStream *output)
{
	LPTSTR p = input;
	EncodeString(p, maxCharsToWrite * sizeof(TCHAR), output, languageID);
}
bool StringCompressor::DecodeString(CString &output, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId)
{
	LPSTR p = output.GetBuffer(maxCharsToWrite * sizeof(TCHAR));
	DecodeString(p, maxCharsToWrite * sizeof(TCHAR), input, languageID);
	output.ReleaseBuffer(0)
}
#endif
// Stack scratch size for DecodeString into std::string / RakString; covers the 0xFFFF cap they use.
static const size_t DECODE_STACK_BLOCK_SIZE = 0x10000;

// Decodes into scratch memory, then hands the decoded C string to assign. Uninitialised stack scratch for the
// common case (RakString and std::string decodes pass 0xFFFF); larger caps fall back to the heap.
template <class Assign>
static bool DecodeToScratch(StringCompressor *compressor, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId, Assign assign)
{
	char stackBlock[DECODE_STACK_BLOCK_SIZE];
	std::unique_ptr<char[]> heapBlock;
	char *destinationBlock = stackBlock;
	if ((size_t) maxCharsToWrite > DECODE_STACK_BLOCK_SIZE)
	{
		heapBlock.reset(new char[(size_t) maxCharsToWrite]);
		destinationBlock = heapBlock.get();
	}
	// DecodeString(char *) does not touch the buffer on every failure path, so never read it uninitialised.
	destinationBlock[0] = 0;
	const bool out = compressor->DecodeString(destinationBlock, maxCharsToWrite, input, languageId);
	assign(destinationBlock);
	return out;
}

#ifdef _STD_STRING_COMPRESSOR
void StringCompressor::EncodeString(const std::string &input, int maxCharsToWrite, MafiaNet::BitStream *output, uint8_t languageId)
{
	EncodeString(input.c_str(), maxCharsToWrite, output, languageId);
}
bool StringCompressor::DecodeString(std::string *output, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId)
{
	if (maxCharsToWrite <= 0)
	{
		output->clear();
		return true;
	}

	return DecodeToScratch(this, maxCharsToWrite, input, languageId, [output](const char *s)
	                       { *output = s; });
}
#endif
void StringCompressor::EncodeString(const RakString *input, int maxCharsToWrite, MafiaNet::BitStream *output, uint8_t languageId)
{
	EncodeString(input->C_String(), maxCharsToWrite, output, languageId);
}
bool StringCompressor::DecodeString(RakString *output, int maxCharsToWrite, MafiaNet::BitStream *input, uint8_t languageId)
{
	if (maxCharsToWrite <= 0)
	{
		output->Clear();
		return true;
	}

	return DecodeToScratch(this, maxCharsToWrite, input, languageId, [output](const char *s)
	                       { *output = s; });
}
