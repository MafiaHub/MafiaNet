/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/huffman_encoding_tree.h"
#include <algorithm>
#include "mafianet/bit_stream.h"
#include "mafianet/assert.h"

using namespace MafiaNet;

namespace
{
// Inserts node before the first entry whose weight is not less than its own, so among equal
// weights the newest node comes first. This is the order the original linked-list insertion
// sort produced; the encodings (and so the wire format) depend on it.
void InsertNodeIntoSortedList(HuffmanEncodingTreeNode *node, std::vector<HuffmanEncodingTreeNode *> &sortedList)
{
	const auto position = std::lower_bound(sortedList.begin(), sortedList.end(), node->weight, [](const HuffmanEncodingTreeNode *entry, uint64_t weight)
	                                       { return entry->weight < weight; });
	sortedList.insert(position, node);
}
} // namespace

HuffmanEncodingTree::HuffmanEncodingTree()
{
	root = nullptr;
}

HuffmanEncodingTree::~HuffmanEncodingTree() = default;

void HuffmanEncodingTree::FreeMemory(void)
{
	nodes.clear();
	for (int i = 0; i < 256; i++)
		encodingTable[i].encoding.reset();
	root = nullptr;
}

// Given a frequency table of 256 elements, generate the tree. A frequency of 0 is treated as 1.
void HuffmanEncodingTree::GenerateFromFrequencyTable(unsigned int frequencyTable[256])
{
	int counter;
	HuffmanEncodingTreeNode *node;
	HuffmanEncodingTreeNode *leafList[256]; // Keep a copy of the pointers to all the leaves so we can generate the encryption table bottom-up, which is easier

	FreeMemory();

	// 256 leaves and 255 internal nodes. Reserving the exact count keeps every node pointer valid.
	nodes.reserve(256 + 255);
	auto newNode = [this]() -> HuffmanEncodingTreeNode *
	{
		nodes.push_back(HuffmanEncodingTreeNode());
		return &nodes.back();
	};

	// 1.  Make 256 trees each with a weight equal to the frequency of the corresponding character
	std::vector<HuffmanEncodingTreeNode *> sortedList;
	sortedList.reserve(256);
	for (counter = 0; counter < 256; counter++)
	{
		node = newNode();
		node->left = nullptr;
		node->right = nullptr;
		node->parent = nullptr;
		node->value = (unsigned char) counter;
		node->weight = frequencyTable[counter];

		if (node->weight == 0)
			node->weight = 1; // 0 weights are illegal

		leafList[counter] = node; // Used later to generate the encryption table

		InsertNodeIntoSortedList(node, sortedList); // Insert and maintain sort order.
	}

	// 2.  While there is more than one tree, take the two smallest trees and merge them so that the two trees are the left and right
	// children of a new node, where the new node has the weight the sum of the weight of the left and right child nodes.
	for (;;)
	{
		HuffmanEncodingTreeNode *lesser = sortedList[0];
		HuffmanEncodingTreeNode *greater = sortedList[1];
		sortedList.erase(sortedList.begin(), sortedList.begin() + 2);
		node = newNode();
		node->value = 0;
		node->left = lesser;
		node->right = greater;
		node->weight = lesser->weight + greater->weight; // Cannot wrap: at most 256 * (2^32 - 1)
		lesser->parent = node;                           // This is done to make generating the encryption table easier
		greater->parent = node;                          // This is done to make generating the encryption table easier

		if (sortedList.empty())
		{
			// 3. Assign the one remaining node in the list to the root node.
			root = node;
			root->parent = nullptr;
			break;
		}

		// Put the new node back into the list at the correct spot to maintain the sort.
		InsertNodeIntoSortedList(node, sortedList);
	}

	bool tempPath[256]; // Maximum path length is 256
	unsigned short tempPathLength;
	HuffmanEncodingTreeNode *currentNode;
	MafiaNet::BitStream bitStream;

	// Generate the encryption table. From before, we have an array of pointers to all the leaves which contain pointers to their parents.
	// This can be done more efficiently but this isn't bad and it's way easier to program and debug

	for (counter = 0; counter < 256; counter++)
	{
		// Already done at the end of the loop and before it!
		tempPathLength = 0;

		// Set the current node at the leaf
		currentNode = leafList[counter];

		do
		{
			if (currentNode->parent->left == currentNode) // We're storing the paths in reverse order.since we are going from the leaf to the root
				tempPath[tempPathLength++] = false;
			else
				tempPath[tempPathLength++] = true;

			currentNode = currentNode->parent;
		}

		while (currentNode != root);

		// Write to the bitstream in the reverse order that we stored the path, which gives us the correct order from the root to the leaf
		while (tempPathLength-- > 0)
		{
			if (tempPath[tempPathLength]) // Write 1's and 0's because writing a bool will write the BitStream TYPE_CHECKING validation bits if that is defined along with the actual data bit, which is not what we want
				bitStream.Write1();
			else
				bitStream.Write0();
		}

		// Read data from the bitstream, which is written to the encoding table in bits and bitlength. Note this function allocates the encodingTable[counter].encoding buffer
		encodingTable[counter].bitLength = (unsigned char) bitStream.CopyData(encodingTable[counter].encoding);

		// Reset the bitstream for the next iteration
		bitStream.Reset();
	}
}

// Pass an array of bytes to array and a preallocated BitStream to receive the output
void HuffmanEncodingTree::EncodeArray(const unsigned char *input, size_t sizeInBytes, MafiaNet::BitStream *output)
{
	unsigned counter;

	// For each input byte, Write out the corresponding series of 1's and 0's that give the encoded representation
	for (counter = 0; counter < sizeInBytes; counter++)
	{
		output->WriteBits(encodingTable[input[counter]].encoding.get(), encodingTable[input[counter]].bitLength, false); // Data is left aligned
	}

	// Byte align the output so the unassigned remaining bits don't equate to some actual value
	if (output->GetNumberOfBitsUsed() % 8 != 0)
	{
		// Find an input that is longer than the remaining bits.  Write out part of it to pad the output to be byte aligned.
		unsigned char remainingBits = (unsigned char) (8 - (output->GetNumberOfBitsUsed() % 8));

		for (counter = 0; counter < 256; counter++)
			if (encodingTable[counter].bitLength > remainingBits)
			{
				output->WriteBits(encodingTable[counter].encoding.get(), remainingBits, false); // Data is left aligned
				break;
			}

#ifdef _DEBUG
		RakAssert(counter != 256); // Given 256 elements, we should always be able to find an input that would be >= 7 bits

#endif
	}
}

size_t HuffmanEncodingTree::DecodeArray(MafiaNet::BitStream *input, BitSize_t sizeInBits, size_t maxCharsToWrite, unsigned char *output)
{
	if (sizeInBits == 0)
		return 0;
	if (root == nullptr)
		return DECODE_ERROR;

	// size_t, not int: the count may exceed maxCharsToWrite and must never turn negative in the caller.
	size_t outputWriteIndex = 0;
	const HuffmanEncodingTreeNode *currentNode = root;

	// For each bit, go left if it is a 0 and right if it is a 1.  When we reach a leaf, that gives us the desired value and we restart from the root
	for (BitSize_t counter = 0; counter < sizeInBits; counter++)
	{
		if (input->ReadBit() == false) // left!
			currentNode = currentNode->left;
		else
			currentNode = currentNode->right;

		// The bits came from the network. A tree built by GenerateFromFrequencyTable() is full, but never follow a missing branch.
		if (currentNode == nullptr)
			return DECODE_ERROR;

		if (currentNode->left == nullptr && currentNode->right == nullptr) // Leaf
		{
			if (outputWriteIndex < maxCharsToWrite)
				output[outputWriteIndex] = currentNode->value;

			outputWriteIndex++;

			currentNode = root;
		}
	}

	return outputWriteIndex;
}

// Pass an array of encoded bytes to array and a preallocated BitStream to receive the output
bool HuffmanEncodingTree::DecodeArray(unsigned char *input, BitSize_t sizeInBits, MafiaNet::BitStream *output)
{
	if (sizeInBits == 0)
		return true;
	if (root == nullptr)
		return false;

	MafiaNet::BitStream bitStream(input, BITS_TO_BYTES(sizeInBits), false);

	const HuffmanEncodingTreeNode *currentNode = root;

	// For each bit, go left if it is a 0 and right if it is a 1.  When we reach a leaf, that gives us the desired value and we restart from the root
	for (BitSize_t counter = 0; counter < sizeInBits; counter++)
	{
		if (bitStream.ReadBit() == false) // left!
			currentNode = currentNode->left;
		else
			currentNode = currentNode->right;

		if (currentNode == nullptr)
			return false;

		if (currentNode->left == nullptr && currentNode->right == nullptr) // Leaf
		{
			output->WriteBits(&(currentNode->value), sizeof(char) * 8, true); // Use WriteBits instead of Write(char) because we want to avoid TYPE_CHECKING
			currentNode = root;
		}
	}
	return true;
}
