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

#include "mafianet/string_table.h"
#include <cstdlib>
#include <string.h>
#include "mafianet/assert.h"
#include <stdio.h>
#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <algorithm>

using namespace MafiaNet;

namespace
{
// First entry not less than str: where str is, or would be inserted.
std::vector<StrAndBool>::const_iterator LowerBoundString(const std::vector<StrAndBool> &list, const char *str)
{
	return std::lower_bound(list.begin(), list.end(), str, [](const StrAndBool &entry, const char *key)
	                        { return StrAndBoolComp(const_cast<char *>(key), entry) > 0; });
}
} // namespace

StringTable *StringTable::instance = 0;
int StringTable::referenceCount = 0;


int MafiaNet::StrAndBoolComp(char *const &key, const StrAndBool &data)
{
	return strcmp(key, (const char *) data.str);
}

StringTable::StringTable()
{
}

StringTable::~StringTable()
{
	for (const StrAndBool &entry : orderedStringList)
	{
		if (entry.b)
			std::free(entry.str);
	}
}

void StringTable::AddReference(void)
{
	if (++referenceCount == 1)
	{
		instance = new StringTable;
	}
}
void StringTable::RemoveReference(void)
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

StringTable *StringTable::Instance(void)
{
	return instance;
}

void StringTable::AddString(const char *str, bool copyString)
{
	StrAndBool sab;
	sab.b = copyString;
	if (copyString)
	{
		sab.str = (char *) std::malloc(strlen(str) + 1);
		strcpy_s(sab.str, strlen(str) + 1, str);
	}
	else
	{
		sab.str = (char *) str;
	}

	const auto position = LowerBoundString(orderedStringList, sab.str);
	if (position != orderedStringList.end() && strcmp(position->str, sab.str) == 0)
	{
		// If it asserts here you are adding duplicate strings.
		RakAssert(0);
		if (copyString)
			std::free(sab.str);
		return;
	}
	orderedStringList.insert(position, sab);

	// If this assert hits you need to increase the range of StringTableType
	RakAssert(orderedStringList.size() < (StringTableType) -1);
}
void StringTable::EncodeString(const char *input, int maxCharsToWrite, MafiaNet::BitStream *output)
{
	// This is fast because the list is kept ordered.
	const auto position = LowerBoundString(orderedStringList, input);
	if (position != orderedStringList.end() && strcmp(position->str, input) == 0)
	{
		output->Write(true);
		output->Write((StringTableType) (position - orderedStringList.begin()));
	}
	else
	{
		LogStringNotFound(input);
		output->Write(false);
		StringCompressor::Instance()->EncodeString(input, maxCharsToWrite, output);
	}
}

bool StringTable::DecodeString(char *output, int maxCharsToWrite, MafiaNet::BitStream *input)
{
	bool hasIndex = false;
	RakAssert(maxCharsToWrite > 0);

	if (maxCharsToWrite == 0)
		return false;
	if (!input->Read(hasIndex))
		return false;
	if (hasIndex == false)
	{
		StringCompressor::Instance()->DecodeString(output, maxCharsToWrite, input);
	}
	else
	{
		StringTableType index;
		if (!input->Read(index))
			return false;
		if (index >= orderedStringList.size())
		{
			// The index came from the network: reject it, never assert. Out of range means AddString was called
			// more times on the remote system than on this one; all systems must add the same strings.
			output[0] = 0;
			return false;
		}

		strncpy_s(output, maxCharsToWrite, orderedStringList[index].str, maxCharsToWrite);
		output[maxCharsToWrite - 1] = 0;
	}

	return true;
}
void StringTable::LogStringNotFound(const char *strName)
{
	(void) strName;

#ifdef _DEBUG
	RAKNET_DEBUG_PRINTF("Efficiency Warning! Unregistered String %s sent to StringTable.\n", strName);
#endif
}
