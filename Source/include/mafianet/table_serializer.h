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

#ifndef __TABLE_SERIALIZER_H
#define __TABLE_SERIALIZER_H

#include "mafianet/table.h"
#include "mafianet/export.h"

#include <vector>

namespace MafiaNet
{
class BitStream;
}

namespace MafiaNet
{

/// Serializes a Table, its rows and filter queries. Every Deserialize* function reads data that
/// can come from a remote peer: it returns false on truncated or malformed input instead of
/// asserting.
class RAK_DLL_EXPORT TableSerializer
{
public:
	static void SerializeTable(MafiaNet::Table *in, MafiaNet::BitStream *out);
	static bool DeserializeTable(unsigned char *serializedTable, unsigned int dataLength, MafiaNet::Table *out);
	static bool DeserializeTable(MafiaNet::BitStream *in, MafiaNet::Table *out);
	static void SerializeColumns(MafiaNet::Table *in, MafiaNet::BitStream *out);
	static void SerializeColumns(MafiaNet::Table *in, MafiaNet::BitStream *out, const std::vector<int> &skipColumnIndices);
	static bool DeserializeColumns(MafiaNet::BitStream *in, MafiaNet::Table *out);
	static void SerializeRow(MafiaNet::Table::Row *in, unsigned keyIn, const std::vector<MafiaNet::Table::ColumnDescriptor> &columns, MafiaNet::BitStream *out);
	static void SerializeRow(MafiaNet::Table::Row *in, unsigned keyIn, const std::vector<MafiaNet::Table::ColumnDescriptor> &columns, MafiaNet::BitStream *out, const std::vector<int> &skipColumnIndices);
	static bool DeserializeRow(MafiaNet::BitStream *in, MafiaNet::Table *out);
	static void SerializeCell(MafiaNet::BitStream *out, MafiaNet::Table::Cell *cell, MafiaNet::Table::ColumnType columnType);
	static bool DeserializeCell(MafiaNet::BitStream *in, MafiaNet::Table::Cell *cell, MafiaNet::Table::ColumnType columnType);
	static void SerializeFilterQuery(MafiaNet::BitStream *in, MafiaNet::Table::FilterQuery *query);
	// Note that this allocates query->cell->c!
	static bool DeserializeFilterQuery(MafiaNet::BitStream *out, MafiaNet::Table::FilterQuery *query);
	static void SerializeFilterQueryList(MafiaNet::BitStream *in, MafiaNet::Table::FilterQuery *query, unsigned int numQueries, unsigned int maxQueries);
	// Note that this allocates queries, cells, and query->cell->c!. Use DeallocateQueryList to free.
	static bool DeserializeFilterQueryList(MafiaNet::BitStream *out, MafiaNet::Table::FilterQuery **query, unsigned int *numQueries, unsigned int maxQueries, int allocateExtraQueries = 0);
	static void DeallocateQueryList(MafiaNet::Table::FilterQuery *query, unsigned int numQueries);
};

} // namespace MafiaNet

#endif
