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

#include "mafianet/table_serializer.h"
#include "mafianet/table.h"
#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"
#include "mafianet/assert.h"

#include <algorithm>
#include <cstdint>
#include <memory>

using namespace MafiaNet;

namespace
{
// Upper bound on rows x columns DeserializeTable builds. A row with no entries is 8 bytes on the
// wire whatever the column count, so without it under 1 MB of input could demand ~1e9 Cells.
constexpr std::uint64_t kMaxDeserializedCells = 1000000;

bool IsSkipped(const std::vector<int> &skipColumnIndices, std::size_t columnIndex)
{
	return std::find(skipColumnIndices.begin(), skipColumnIndices.end(), (int) columnIndex) != skipColumnIndices.end();
}
} // namespace

void TableSerializer::SerializeTable(MafiaNet::Table *in, MafiaNet::BitStream *out)
{
	const std::vector<MafiaNet::Table::ColumnDescriptor> &columns = in->GetColumns();
	SerializeColumns(in, out);
	out->Write((unsigned) in->GetRows().size());
	for (const auto &entry : in->GetRows())
		SerializeRow(entry.second.get(), entry.first, columns, out);
}
void TableSerializer::SerializeColumns(MafiaNet::Table *in, MafiaNet::BitStream *out)
{
	const std::vector<MafiaNet::Table::ColumnDescriptor> &columns = in->GetColumns();
	out->Write((unsigned) columns.size());
	for (const auto &column : columns)
	{
		StringCompressor::Instance()->EncodeString(column.columnName, _TABLE_MAX_COLUMN_NAME_LENGTH, out);
		out->Write((unsigned char) column.columnType);
	}
}
void TableSerializer::SerializeColumns(MafiaNet::Table *in, MafiaNet::BitStream *out, const std::vector<int> &skipColumnIndices)
{
	const std::vector<MafiaNet::Table::ColumnDescriptor> &columns = in->GetColumns();
	out->Write((unsigned) (columns.size() - skipColumnIndices.size()));
	for (std::size_t i = 0; i < columns.size(); i++)
	{
		if (IsSkipped(skipColumnIndices, i) == false)
		{
			StringCompressor::Instance()->EncodeString(columns[i].columnName, _TABLE_MAX_COLUMN_NAME_LENGTH, out);
			out->Write((unsigned char) columns[i].columnType);
		}
	}
}
bool TableSerializer::DeserializeTable(unsigned char *serializedTable, unsigned int dataLength, MafiaNet::Table *out)
{
	MafiaNet::BitStream in((unsigned char *) serializedTable, dataLength, false);
	return DeserializeTable(&in, out);
}
bool TableSerializer::DeserializeTable(MafiaNet::BitStream *in, MafiaNet::Table *out)
{
	if (DeserializeColumns(in, out) == false)
		return false;

	unsigned rowSize;
	if (in->Read(rowSize) == false || rowSize > 100000)
		return false; // Truncated, or hacker crash prevention
	// Every row costs a key and an entry count on the wire (64 bits) but one Cell per column in
	// memory: refuse a row count the input cannot hold, and a table too large to build.
	if (static_cast<std::uint64_t>(rowSize) * 64 > in->GetNumberOfUnreadBits() ||
	    static_cast<std::uint64_t>(rowSize) * out->GetColumns().size() > kMaxDeserializedCells)
		return false;

	for (unsigned rowIndex = 0; rowIndex < rowSize; rowIndex++)
	{
		if (DeserializeRow(in, out) == false)
			return false;
	}
	return true;
}
bool TableSerializer::DeserializeColumns(MafiaNet::BitStream *in, MafiaNet::Table *out)
{
	unsigned columnSize;
	unsigned char columnType;
	char columnName[_TABLE_MAX_COLUMN_NAME_LENGTH];
	if (in->Read(columnSize) == false || columnSize > 10000)
		return false; // Hacker crash prevention

	out->Clear();
	for (unsigned i = 0; i < columnSize; i++)
	{
		if (StringCompressor::Instance()->DecodeString(columnName, 32, in) == false)
			return false;
		if (in->Read(columnType) == false || columnType > MafiaNet::Table::POINTER)
			return false;
		out->AddColumn(columnName, (MafiaNet::Table::ColumnType) columnType);
	}
	return true;
}
void TableSerializer::SerializeRow(MafiaNet::Table::Row *in, unsigned keyIn, const std::vector<MafiaNet::Table::ColumnDescriptor> &columns, MafiaNet::BitStream *out)
{
	out->Write(keyIn);
	unsigned int columnsSize = (unsigned int) columns.size();
	out->Write(columnsSize);
	for (unsigned cellIndex = 0; cellIndex < columnsSize; cellIndex++)
	{
		out->Write(cellIndex);
		SerializeCell(out, in->cells[cellIndex].get(), columns[cellIndex].columnType);
	}
}
void TableSerializer::SerializeRow(MafiaNet::Table::Row *in, unsigned keyIn, const std::vector<MafiaNet::Table::ColumnDescriptor> &columns, MafiaNet::BitStream *out, const std::vector<int> &skipColumnIndices)
{
	out->Write(keyIn);
	unsigned int numEntries = 0;
	for (std::size_t cellIndex = 0; cellIndex < columns.size(); cellIndex++)
	{
		if (IsSkipped(skipColumnIndices, cellIndex) == false)
			numEntries++;
	}
	out->Write(numEntries);

	for (unsigned cellIndex = 0; cellIndex < (unsigned) columns.size(); cellIndex++)
	{
		if (IsSkipped(skipColumnIndices, cellIndex) == false)
		{
			out->Write(cellIndex);
			SerializeCell(out, in->cells[cellIndex].get(), columns[cellIndex].columnType);
		}
	}
}
bool TableSerializer::DeserializeRow(MafiaNet::BitStream *in, MafiaNet::Table *out)
{
	const std::vector<MafiaNet::Table::ColumnDescriptor> &columns = out->GetColumns();
	unsigned key;
	if (in->Read(key) == false)
		return false;
	MafiaNet::Table::Row *row = out->AddRow(key);
	if (row == nullptr)
		return false; // Duplicate row id

	unsigned numEntries;
	bool ok = in->Read(numEntries);
	for (unsigned cnt = 0; ok && cnt < numEntries; cnt++)
	{
		unsigned cellIndex;
		ok = in->Read(cellIndex) && cellIndex < columns.size() && DeserializeCell(in, row->cells[cellIndex].get(), columns[cellIndex].columnType);
	}
	if (ok == false)
	{
		out->RemoveRow(key);
		return false;
	}
	return true;
}
void TableSerializer::SerializeCell(MafiaNet::BitStream *out, MafiaNet::Table::Cell *cell, MafiaNet::Table::ColumnType columnType)
{
	out->Write(cell->isEmpty);
	if (cell->isEmpty == false)
	{
		if (columnType == MafiaNet::Table::NUMERIC)
		{
			out->Write(cell->i);
		}
		else if (columnType == MafiaNet::Table::STRING)
		{
			StringCompressor::Instance()->EncodeString(cell->c, 65535, out);
		}
		else if (columnType == MafiaNet::Table::POINTER)
		{
			out->Write(cell->ptr);
		}
		else
		{
			// Binary. A zero-length blob is written as length 0 and no bytes.
			RakAssert(columnType == MafiaNet::Table::BINARY);
			const unsigned binaryLength = cell->c && cell->i > 0 ? (unsigned) cell->i : 0;
			out->Write(binaryLength);
			out->WriteAlignedBytes((const unsigned char *) cell->c, binaryLength);
		}
	}
}
bool TableSerializer::DeserializeCell(MafiaNet::BitStream *in, MafiaNet::Table::Cell *cell, MafiaNet::Table::ColumnType columnType)
{
	bool isEmpty = false;
	double value;
	void *ptr;
	char tempString[65535];
	cell->Clear();

	if (in->Read(isEmpty) == false)
		return false;
	if (isEmpty == false)
	{
		if (columnType == MafiaNet::Table::NUMERIC)
		{
			if (in->Read(value) == false)
				return false;
			cell->Set(value);
		}
		else if (columnType == MafiaNet::Table::STRING)
		{
			if (StringCompressor::Instance()->DecodeString(tempString, 65535, in) == false)
				return false;
			cell->Set(tempString);
		}
		else if (columnType == MafiaNet::Table::POINTER)
		{
			if (in->Read(ptr) == false)
				return false;
			cell->SetPtr(ptr);
		}
		else if (columnType == MafiaNet::Table::BINARY)
		{
			unsigned binaryLength;
			if (in->Read(binaryLength) == false || binaryLength > 10000000)
				return false; // Sanity check to max binary cell of 10 megabytes
			in->AlignReadToByteBoundary();
			if (BITS_TO_BYTES(in->GetNumberOfUnreadBits()) < (BitSize_t) binaryLength)
				return false;
			cell->Set((char *) in->GetData() + BITS_TO_BYTES(in->GetReadOffset()), (int) binaryLength);
			in->IgnoreBits(BYTES_TO_BITS((int) binaryLength));
		}
		else
			return false; // Unknown column type
	}
	return true;
}
void TableSerializer::SerializeFilterQuery(MafiaNet::BitStream *in, MafiaNet::Table::FilterQuery *query)
{
	StringCompressor::Instance()->EncodeString(query->columnName, _TABLE_MAX_COLUMN_NAME_LENGTH, in, 0);
	in->WriteCompressed(query->columnIndex);
	in->Write((unsigned char) query->operation);
	in->Write(query->cellValue->isEmpty);
	if (query->cellValue->isEmpty == false)
	{
		in->Write(query->cellValue->i);
		in->WriteAlignedBytesSafe((const char *) query->cellValue->c, (unsigned int) query->cellValue->i, 10000000); // Sanity check to max binary cell of 10 megabytes
		in->Write(query->cellValue->ptr);
	}
}
bool TableSerializer::DeserializeFilterQuery(MafiaNet::BitStream *out, MafiaNet::Table::FilterQuery *query)
{
	RakAssert(query->cellValue);
	if (query->cellValue == nullptr)
		return false;
	if (StringCompressor::Instance()->DecodeString(query->columnName, _TABLE_MAX_COLUMN_NAME_LENGTH, out, 0) == false)
		return false;
	if (out->ReadCompressed(query->columnIndex) == false)
		return false;
	unsigned char op;
	if (out->Read(op) == false || op > MafiaNet::Table::QF_NOT_EMPTY)
		return false;
	query->operation = (MafiaNet::Table::FilterQueryType) op;
	query->cellValue->Clear();
	bool isEmpty;
	if (out->Read(isEmpty) == false)
		return false;
	if (isEmpty == false)
	{
		// HACK - cellValue->i is used for integer, character, and binary data. However, for character and binary c will be 0. So use that to determine if the data was integer or not.
		double numeric;
		if (out->Read(numeric) == false)
			return false;
		unsigned int inputLength;
		std::unique_ptr<unsigned char[]> binary;
		if (out->ReadAlignedBytesSafeAlloc(binary, inputLength, 10000000) == false) // Sanity check to max binary cell of 10 megabytes
			return false;
		void *ptr;
		if (out->Read(ptr) == false)
			return false;
		if (binary)
		{
			// The cell owns (and std::free()s) its own copy of the bytes; it also sets i to the length.
			query->cellValue->Set((const char *) binary.get(), (int) inputLength);
		}
		else
		{
			query->cellValue->i = numeric;
			query->cellValue->isEmpty = false;
		}
		query->cellValue->ptr = ptr;
	}
	return true;
}
void TableSerializer::SerializeFilterQueryList(MafiaNet::BitStream *in, MafiaNet::Table::FilterQuery *query, unsigned int numQueries, unsigned int maxQueries)
{
	(void) maxQueries;
	in->Write((bool) (query && numQueries > 0));
	if (query == 0 || numQueries <= 0)
		return;

	RakAssert(numQueries <= maxQueries);
	in->WriteCompressed(numQueries);
	unsigned i;
	for (i = 0; i < numQueries; i++)
	{
		SerializeFilterQuery(in, query);
	}
}
bool TableSerializer::DeserializeFilterQueryList(MafiaNet::BitStream *out, MafiaNet::Table::FilterQuery **query, unsigned int *numQueries, unsigned int maxQueries, int allocateExtraQueries)
{
	// Leave something DeallocateQueryList() accepts behind on every early return.
	*query = 0;
	*numQueries = 0;

	bool anyQueries = false;
	if (out->Read(anyQueries) == false)
		return false;
	if (anyQueries == false)
	{
		if (allocateExtraQueries > 0)
			*query = new MafiaNet::Table::FilterQuery[allocateExtraQueries];
		return true;
	}
	unsigned int count;
	if (out->ReadCompressed(count) == false || count > maxQueries)
		return false; // Truncated, or more queries than the caller accepts
	if (count == 0)
		return true;

	*query = new MafiaNet::Table::FilterQuery[count + (allocateExtraQueries > 0 ? (unsigned int) allocateExtraQueries : 0u)];
	*numQueries = count;
	MafiaNet::Table::FilterQuery *queryPtr = *query;

	// Every query gets a cell before any is read, so a caller that ignores a false return never
	// meets a null cellValue.
	for (unsigned i = 0; i < count; i++)
		queryPtr[i].cellValue = new MafiaNet::Table::Cell;
	for (unsigned i = 0; i < count; i++)
	{
		if (DeserializeFilterQuery(out, queryPtr + i) == false)
			return false;
	}
	return true;
}
void TableSerializer::DeallocateQueryList(MafiaNet::Table::FilterQuery *query, unsigned int numQueries)
{
	if (query == 0 || numQueries == 0)
		return;

	unsigned i;
	for (i = 0; i < numQueries; i++)
		delete query[i].cellValue;
	delete[] query;
}
