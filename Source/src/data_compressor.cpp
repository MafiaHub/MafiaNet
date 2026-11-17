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

#include "mafianet/data_compressor.h"
#include <cstdlib>
#include <utility>
#include "mafianet/huffman_encoding_tree.h"
#include "mafianet/assert.h"
#include <string.h> // Use string.h rather than memory.h for a console

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(DataCompressor, DataCompressor)

void DataCompressor::Compress(unsigned char *userData, unsigned sizeInBytes, MafiaNet::BitStream *output)
{
	// Don't use this for small files as you will just make them bigger!
	RakAssert(sizeInBytes > 2048);

	unsigned int frequencyTable[256];
	unsigned int i;
	memset(frequencyTable, 0, 256 * sizeof(unsigned int));
	for (i = 0; i < sizeInBytes; i++)
		++frequencyTable[userData[i]];
	HuffmanEncodingTree tree;
	BitSize_t writeOffset1, writeOffset2, bitsUsed1, bitsUsed2;
	tree.GenerateFromFrequencyTable(frequencyTable);
	output->WriteCompressed(sizeInBytes);
	for (i = 0; i < 256; i++)
		output->WriteCompressed(frequencyTable[i]);
	output->AlignWriteToByteBoundary();
	writeOffset1 = output->GetWriteOffset();
	output->Write((unsigned int) 0); // Dummy value
	bitsUsed1 = output->GetNumberOfBitsUsed();
	tree.EncodeArray(userData, sizeInBytes, output);
	bitsUsed2 = output->GetNumberOfBitsUsed();
	writeOffset2 = output->GetWriteOffset();
	output->SetWriteOffset(writeOffset1);
	output->Write(bitsUsed2 - bitsUsed1); // Go back and write how many bits were used for the encoding
	output->SetWriteOffset(writeOffset2);
}

unsigned DataCompressor::DecompressAndAllocate(MafiaNet::BitStream *input, std::unique_ptr<unsigned char[]> &output)
{
	output.reset();

	HuffmanEncodingTree tree;
	unsigned int bitsUsed = 0, destinationSizeInBytes = 0;
	unsigned int frequencyTable[256];
	unsigned i;

	// Everything here is read from the network: fail (never assert) on malformed input.
	if (input->ReadCompressed(destinationSizeInBytes) == false)
		return 0;
	for (i = 0; i < 256; i++)
	{
		if (input->ReadCompressed(frequencyTable[i]) == false)
			return 0;
	}
	input->AlignReadToByteBoundary();
	if (input->Read(bitsUsed) == false)
		return 0;

	// The payload must actually be present, and every decoded byte costs at least one bit
	// of it, so a declared size above bitsUsed cannot be honest. This bounds the allocation
	// by the size of the stream that was received.
	if (bitsUsed > input->GetNumberOfUnreadBits() || destinationSizeInBytes > bitsUsed)
		return 0;
	if (destinationSizeInBytes == 0)
		return 0;

	std::unique_ptr<unsigned char[]> buffer(new unsigned char[destinationSizeInBytes]);
	tree.GenerateFromFrequencyTable(frequencyTable);
	const size_t decodedBytes = tree.DecodeArray(input, bitsUsed, destinationSizeInBytes, buffer.get());
	if (decodedBytes == HuffmanEncodingTree::DECODE_ERROR || decodedBytes != destinationSizeInBytes)
		return 0;
	output = std::move(buffer);
	return destinationSizeInBytes;
}
