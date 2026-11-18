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
#include "mafianet/assert.h"
#include <stdio.h>
#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"

#include <algorithm>
#include <limits>
#include <string_view>

using namespace MafiaNet;

StringTable *StringTable::instance = 0;
int StringTable::referenceCount = 0;


StringTable::StringTable() = default;
StringTable::~StringTable() = default;

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

void StringTable::AddString(const char *str)
{
	const std::string_view key(str);
	const auto position = std::lower_bound(orderedStringList.begin(), orderedStringList.end(), key);
	if (position != orderedStringList.end() && *position == key)
	{
		RakAssert(0); // Duplicate registration.
		return;
	}
	// The current size is the highest index the table would need after insertion.
	if (orderedStringList.size() > (std::numeric_limits<StringTableType>::max)())
		return;

	orderedStringList.emplace(position, key);
}
void StringTable::EncodeString(const char *input, int maxCharsToWrite, MafiaNet::BitStream *output)
{
	const std::string_view key(input ? input : "");
	const auto position = std::lower_bound(orderedStringList.begin(), orderedStringList.end(), key);
	if (position != orderedStringList.end() && *position == key)
	{
		output->Write(true);
		output->Write((StringTableType) (position - orderedStringList.begin()));
	}
	else
	{
		LogStringNotFound(key.data());
		output->Write(false);
		StringCompressor::Instance()->EncodeString(key, maxCharsToWrite, output);
	}
}

bool StringTable::DecodeString(char *output, int maxCharsToWrite, MafiaNet::BitStream *input)
{
	bool hasIndex = false;
	RakAssert(maxCharsToWrite > 0);

	if (maxCharsToWrite <= 0)
		return false;
	if (!input->Read(hasIndex))
		return false;
	if (!hasIndex)
		return StringCompressor::Instance()->DecodeString(output, maxCharsToWrite, input);

	StringTableType index;
	if (!input->Read(index))
		return false;
	if (index >= orderedStringList.size())
	{
		output[0] = 0;
		return false;
	}

	const size_t copied = orderedStringList[index].copy(output, static_cast<size_t>(maxCharsToWrite - 1));
	output[copied] = '\0';

	return true;
}
void StringTable::LogStringNotFound(const char *strName)
{
	(void) strName;

#ifdef _DEBUG
	RAKNET_DEBUG_PRINTF("Efficiency Warning! Unregistered String %s sent to StringTable.\n", strName);
#endif
}
