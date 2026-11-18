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

/// \file huffman_encoding_tree.h
/// \brief \b [Internal] Generates a huffman encoding tree, used for string and global compression.
///


#ifndef __HUFFMAN_ENCODING_TREE
#define __HUFFMAN_ENCODING_TREE

#include "mafianet/huffman_encoding_tree_node.h"
#include "mafianet/bit_stream.h"
#include "mafianet/export.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace MafiaNet
{

/// This generates special cases of the huffman encoding tree using 8 bit keys with the additional condition that unused combinations of 8 bits are treated as a frequency of 1
class RAK_DLL_EXPORT HuffmanEncodingTree
{

public:
	HuffmanEncodingTree();
	~HuffmanEncodingTree();

	// The nodes point at each other; a copy would alias the original's tree.
	HuffmanEncodingTree(const HuffmanEncodingTree &) = delete;
	HuffmanEncodingTree &operator=(const HuffmanEncodingTree &) = delete;

	/// Returned by DecodeArray() when the tree cannot decode the bits: no tree was generated, or a branch is missing.
	static constexpr size_t DECODE_ERROR = static_cast<size_t>(-1);

	/// \brief Pass an array of bytes to array and a preallocated BitStream to receive the output.
	/// \param [in] input Array of bytes to encode
	/// \param [in] sizeInBytes size of \a input
	/// \param [out] output The bitstream to write to
	void EncodeArray(const unsigned char *input, size_t sizeInBytes, MafiaNet::BitStream *output);

	/// \brief Decodes \a sizeInBits bits encoded by EncodeArray().
	/// \details Writes at most \a maxCharsToWrite characters to \a output; characters decoded past that are counted but dropped.
	/// The input is untrusted: decoding stops (it never asserts) if the tree cannot follow the bits.
	/// \return The number of characters the bits decode to, which may exceed \a maxCharsToWrite, or DECODE_ERROR.
	size_t DecodeArray(MafiaNet::BitStream *input, BitSize_t sizeInBits, size_t maxCharsToWrite, unsigned char *output);
	/// \brief Decodes \a sizeInBits bits of \a input encoded by EncodeArray(), appending the characters to \a output.
	/// \return false if the tree cannot follow the bits (no tree was generated); output then holds what was decoded so far.
	bool DecodeArray(unsigned char *input, BitSize_t sizeInBits, MafiaNet::BitStream *output);

	/// \brief Given a frequency table of 256 elements, generate the tree. A frequency of 0 is treated as 1.
	void GenerateFromFrequencyTable(unsigned int frequencyTable[256]);

	/// \brief Free the memory used by the tree.
	void FreeMemory(void);

private:
	/// Storage for every node of the tree (256 leaves and 255 internal nodes). Reserved up front, so node pointers stay valid.
	std::vector<HuffmanEncodingTreeNode> nodes;

	/// The root node of the tree, or nullptr before GenerateFromFrequencyTable()
	HuffmanEncodingTreeNode *root;

	/// Used to hold bit encoding for one character
	struct CharacterEncoding
	{
		std::unique_ptr<unsigned char[]> encoding;
		unsigned short bitLength;
	};

	CharacterEncoding encodingTable[256];
};

} // namespace MafiaNet

#endif
