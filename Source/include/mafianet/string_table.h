/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017-2019, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief A simple class to encode and decode known strings based on a lookup table.  Similar to the StringCompressor class.
///


#ifndef __STRING_TABLE_H
#define __STRING_TABLE_H

#include "mafianet/export.h"

#include <string>
#include <vector>

/// Forward declaration
namespace MafiaNet
{
class BitStream;
};

/// StringTableType should be the smallest type possible, or else it defeats the purpose of the StringTable class, which is to save bandwidth.
typedef unsigned char StringTableType;

namespace MafiaNet
{
/// \brief Encodes registered strings by index and compresses unregistered strings with StringCompressor.
/// Register the same set of strings on all peers before exchanging messages. Indices follow bytewise
/// sort order, so inserting a new string can shift the indices of existing entries.
class RAK_DLL_EXPORT StringTable
{
public:
	// Destructor
	~StringTable();

	/// static function because only static functions can access static members
	/// The RakPeer constructor adds a reference to this class, so don't call this until an instance of RakPeer exists, or unless you call AddReference yourself.
	/// \return the unique instance of the StringTable
	static StringTable *Instance(void);

	/// Register a NUL-terminated string. The table owns a copy after this call.
	void AddString(const char *str);

	/// Writes input to output, compressed.  Takes care of the null-terminator for you.
	/// Relies on the StringCompressor class, which is automatically reference counted in the constructor and destructor in RakPeer.  You can call the reference counting functions yourself if you wish too.
	/// \param[in] input Pointer to an ASCII string
	/// \param[in] maxCharsToWrite The size of \a input
	/// \param[out] output The bitstream to write the compressed string to
	void EncodeString(const char *input, int maxCharsToWrite, MafiaNet::BitStream *output);

	/// Writes input to output, uncompressed.  Takes care of the null-terminator for you.
	/// Relies on the StringCompressor class, which is automatically reference counted in the constructor and destructor in RakPeer.  You can call the reference counting functions yourself if you wish too.
	/// \param[out] output A block of bytes to receive the output
	/// \param[in] maxCharsToWrite Size, in bytes, of \a output .  A null-terminator will always be appended to the output string.  If the maxCharsToWrite is not large enough, the string will be truncated.
	/// \param[in] input The bitstream containing the compressed string
	bool DecodeString(char *output, int maxCharsToWrite, MafiaNet::BitStream *input);

	/// Used so I can allocate and deallocate this singleton at runtime
	static void AddReference(void);

	/// Used so I can allocate and deallocate this singleton at runtime
	static void RemoveReference(void);

	StringTable();

	StringTable(const StringTable &) = delete;
	StringTable &operator=(const StringTable &) = delete;

protected:
	/// Called when you mess up and send a string using this class that was not registered with AddString
	/// \param[in] maxCharsToWrite Size, in bytes, of \a output .  A null-terminator will always be appended to the output string.  If the maxCharsToWrite is not large enough, the string will be truncated.
	void LogStringNotFound(const char *strName);

	/// Singleton instance
	static StringTable *instance;
	static int referenceCount;

	/// Sorted lexicographically by byte value. The position of a string is its index on the wire.
	std::vector<std::string> orderedStringList;
};
}


#endif
