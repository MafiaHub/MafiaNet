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

#include "mafianet/native_feature_includes.h"
#include <cstdlib>
#if _RAKNET_SUPPORT_CloudClient == 1 || _RAKNET_SUPPORT_CloudServer == 1

#include "mafianet/cloud_common.h"
#include "mafianet/bit_stream.h"

using namespace MafiaNet;

int MafiaNet::CloudKeyComp(const CloudKey &key, const CloudKey &data)
{
	if (key.primaryKey < data.primaryKey)
		return -1;
	if (key.primaryKey > data.primaryKey)
		return 1;
	if (key.secondaryKey < data.secondaryKey)
		return -1;
	if (key.secondaryKey > data.secondaryKey)
		return 1;
	return 0;
}

CloudQueryRow *CloudAllocator::AllocateCloudQueryRow(void)
{
	return new CloudQueryRow();
}
void CloudAllocator::DeallocateCloudQueryRow(CloudQueryRow *row)
{
	delete row;
}
unsigned char *CloudAllocator::AllocateRowData(uint32_t bytesNeededForData)
{
	return (unsigned char *) std::malloc(bytesNeededForData);
}
void CloudAllocator::DeallocateRowData(void *data)
{
	std::free(data);
}
bool CloudKey::Serialize(bool writeToBitstream, BitStream *bitStream)
{
	const bool primaryOk = bitStream->Serialize(writeToBitstream, primaryKey);
	const bool secondaryOk = bitStream->Serialize(writeToBitstream, secondaryKey);
	return primaryOk && secondaryOk;
}
void CloudQuery::Serialize(bool writeToBitstream, BitStream *bitStream)
{
	bool startingRowIndexIsZero = 0;
	bool maxRowsToReturnIsZero = 0;
	startingRowIndexIsZero = startingRowIndex == 0;
	maxRowsToReturnIsZero = maxRowsToReturn == 0;
	bitStream->Serialize(writeToBitstream, startingRowIndexIsZero);
	bitStream->Serialize(writeToBitstream, maxRowsToReturnIsZero);
	bitStream->Serialize(writeToBitstream, subscribeToResults);
	if (startingRowIndexIsZero == false)
		bitStream->Serialize(writeToBitstream, startingRowIndex);
	if (maxRowsToReturnIsZero == false)
		bitStream->Serialize(writeToBitstream, maxRowsToReturn);
	RakAssert(keys.size() < (uint16_t) -1);
	uint16_t numKeys = (uint16_t) keys.size();
	bitStream->Serialize(writeToBitstream, numKeys);
	if (writeToBitstream)
	{
		for (uint16_t i = 0; i < numKeys; i++)
		{
			keys[i].Serialize(true, bitStream);
		}
	}
	else
	{
		CloudKey cmdk;
		for (uint16_t i = 0; i < numKeys; i++)
		{
			cmdk.Serialize(false, bitStream);
			keys.push_back(cmdk);
		}
	}
}
bool CloudQueryRow::Serialize(bool writeToBitstream, BitStream *bitStream, CloudAllocator *allocator)
{
	if (writeToBitstream)
	{
		key.Serialize(true, bitStream);
		bitStream->Write(serverSystemAddress);
		bitStream->Write(clientSystemAddress);
		bitStream->Write(serverGUID);
		bitStream->Write(clientGUID);
		bitStream->Write(length);
		bitStream->WriteAlignedBytes((const unsigned char *) data, length);
		return true;
	}

	// Everything below is peer input.
	data = 0;
	if (key.Serialize(false, bitStream) == false ||
	    bitStream->Read(serverSystemAddress) == false ||
	    bitStream->Read(clientSystemAddress) == false ||
	    bitStream->Read(serverGUID) == false ||
	    bitStream->Read(clientGUID) == false ||
	    bitStream->Read(length) == false)
	{
		length = 0;
		return false;
	}
	if (length == 0)
		return true;
	// Never allocate more than the stream can still hold.
	if (BITS_TO_BYTES(bitStream->GetNumberOfUnreadBits()) < length)
	{
		length = 0;
		return false;
	}
	data = allocator->AllocateRowData(length);
	if (data == 0 || bitStream->ReadAlignedBytes(data, length) == false)
	{
		if (data)
			allocator->DeallocateRowData(data);
		data = 0;
		length = 0;
		return false;
	}
	return true;
}
void CloudQueryResult::SerializeHeader(bool writeToBitstream, BitStream *bitStream)
{
	cloudQuery.Serialize(writeToBitstream, bitStream);
	bitStream->Serialize(writeToBitstream, subscribeToResults);
}
void CloudQueryResult::SerializeNumRows(bool writeToBitstream, uint32_t &numRows, BitStream *bitStream)
{
	bitStream->Serialize(writeToBitstream, numRows);
}
void CloudQueryResult::SerializeCloudQueryRows(bool writeToBitstream, uint32_t &numRows, BitStream *bitStream, CloudAllocator *allocator)
{
	if (writeToBitstream)
	{
		for (uint32_t i = 0; i < numRows; i++)
		{
			rowsReturned[i]->Serialize(true, bitStream, allocator);
		}
	}
	else
	{
		CloudQueryRow *cmdr;
		// numRows is peer input and may claim up to 2^32 - 1 rows: stop at the first row the
		// stream does not hold whole. Every row read consumes bits, so the stream bounds the loop.
		for (uint32_t i = 0; i < numRows; i++)
		{
			if (bitStream->GetNumberOfUnreadBits() == 0)
			{
				numRows = i;
				return;
			}
			cmdr = allocator->AllocateCloudQueryRow();
			if (cmdr == 0)
			{
				numRows = i;
				return;
			}
			if (cmdr->Serialize(false, bitStream, allocator) == false)
			{
				allocator->DeallocateCloudQueryRow(cmdr);
				numRows = i;
				return;
			}
			rowsReturned.push_back(cmdr);
		}
	}
}
void CloudQueryResult::Serialize(bool writeToBitstream, BitStream *bitStream, CloudAllocator *allocator)
{
	SerializeHeader(writeToBitstream, bitStream);
	uint32_t numRows = (uint32_t) rowsReturned.size();
	SerializeNumRows(writeToBitstream, numRows, bitStream);
	SerializeCloudQueryRows(writeToBitstream, numRows, bitStream, allocator);
}

#endif // #if _RAKNET_SUPPORT_CloudMemoryClient==1 || _RAKNET_SUPPORT_CloudMemoryServer==1
