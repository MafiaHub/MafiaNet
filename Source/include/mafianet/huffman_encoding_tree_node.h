/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

/// \file
/// \brief \b [Internal] A single node in the Huffman Encoding Tree.
///

#ifndef __HUFFMAN_ENCODING_TREE_NODE
#define __HUFFMAN_ENCODING_TREE_NODE

#include <cstdint>

namespace MafiaNet
{

struct HuffmanEncodingTreeNode
{
	unsigned char value;
	/// Sum of the leaf frequencies below this node. 64 bits wide so that 256 frequencies of up to
	/// 2^32-1 each (read from the network by DataCompressor) cannot wrap and skew the tree.
	uint64_t weight;
	HuffmanEncodingTreeNode *left;
	HuffmanEncodingTreeNode *right;
	HuffmanEncodingTreeNode *parent;
};

} // namespace MafiaNet

#endif
