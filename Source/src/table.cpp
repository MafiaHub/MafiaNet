/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/table.h"

#include "mafianet/assert.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

using namespace MafiaNet;

namespace
{
// Allocates length bytes plus one zero byte that is not part of the data: a binary cell that
// ends up compared as a string (strcmp in QueryTable/SortTable) then stops at its own buffer.
char *AllocateCellBuffer(const char *source, std::size_t length)
{
	char *buffer = static_cast<char *>(std::malloc(length + 1));
	if (buffer == nullptr)
		return nullptr;
	if (length > 0)
		std::memcpy(buffer, source, length);
	buffer[length] = 0;
	return buffer;
}

// Length of the buffer a cell owns: i holds it for string and binary cells.
std::size_t OwnedLength(const Table::Cell &cell)
{
	return cell.i > 0.0 ? static_cast<std::size_t>(cell.i) : 0;
}

const Table::Cell *CellOf(const Table::Cell *cell)
{
	return cell;
}
const Table::Cell *CellOf(const std::unique_ptr<Table::Cell> &cell)
{
	return cell.get();
}

const char *StringOrEmpty(const char *s)
{
	return s ? s : "";
}
} // namespace

Table::Cell::Cell()
{
}
Table::Cell::~Cell()
{
	Clear();
}
Table::Cell &Table::Cell::operator=(const Table::Cell &input)
{
	if (this == &input)
		return *this;
	isEmpty = input.isEmpty;
	i = input.i;
	ptr = input.ptr;
	std::free(c);
	c = input.c ? AllocateCellBuffer(input.c, OwnedLength(input)) : nullptr;
	return *this;
}
Table::Cell::Cell(const Table::Cell &input)
    : isEmpty(input.isEmpty), i(input.i), c(input.c ? AllocateCellBuffer(input.c, OwnedLength(input)) : nullptr), ptr(input.ptr)
{
}
void Table::Cell::Set(double input)
{
	Clear();
	i = input;
	ptr = nullptr;
	isEmpty = false;
}
void Table::Cell::Set(unsigned int input)
{
	Set((int) input);
}
void Table::Cell::Set(int input)
{
	Set((double) input);
}

void Table::Cell::Set(const char *input)
{
	// Copy before freeing: input may point into this cell's own buffer.
	const std::size_t length = input ? std::strlen(input) + 1 : 0;
	char *buffer = input ? AllocateCellBuffer(input, length) : nullptr;
	Clear();
	c = buffer;
	i = (double) length;
	ptr = nullptr;
	isEmpty = false;
}
void Table::Cell::Set(const char *input, int inputLength)
{
	// A null input or a non-positive length is a zero-length blob. With a non-null input it still
	// owns an (empty) buffer.
	const std::size_t length = input && inputLength > 0 ? (std::size_t) inputLength : 0;
	char *buffer = input ? AllocateCellBuffer(input, length) : nullptr;
	Clear();
	c = buffer;
	i = (double) length;
	ptr = nullptr;
	isEmpty = false;
}
void Table::Cell::SetPtr(void *p)
{
	Clear();
	ptr = p;
	isEmpty = false;
}
void Table::Cell::Get(int *output)
{
	RakAssert(isEmpty == false);
	*output = (int) i;
}
void Table::Cell::Get(double *output)
{
	RakAssert(isEmpty == false);
	*output = i;
}
void Table::Cell::Get(char *output)
{
	RakAssert(isEmpty == false);
	strcpy(output, StringOrEmpty(c));
}
void Table::Cell::Get(char *output, size_t outputLength)
{
	RakAssert(isEmpty == false);
	strcpy_s(output, outputLength, StringOrEmpty(c));
}
void Table::Cell::Get(char *output, int *outputLength)
{
	RakAssert(isEmpty == false);
	const std::size_t length = c ? OwnedLength(*this) : 0;
	if (length > 0)
		std::memcpy(output, c, length);
	if (outputLength)
		*outputLength = (int) length;
}
MafiaNet::RakString Table::Cell::ToString(ColumnType columnType)
{
	if (isEmpty)
		return MafiaNet::RakString();

	if (columnType == NUMERIC)
	{
		return MafiaNet::RakString("%f", i);
	}
	else if (columnType == STRING)
	{
		return MafiaNet::RakString(c);
	}
	else if (columnType == BINARY)
	{
		return MafiaNet::RakString("<Binary>");
	}
	else if (columnType == POINTER)
	{
		return MafiaNet::RakString("%p", ptr);
	}

	return MafiaNet::RakString();
}
Table::Cell::Cell(double numericValue, char *charValue, void *inPtr, ColumnType type)
{
	SetByType(numericValue, charValue, inPtr, type);
}
void Table::Cell::SetByType(double numericValue, char *charValue, void *inPtr, ColumnType type)
{
	if (type == NUMERIC)
	{
		Set(numericValue);
	}
	else if (type == STRING)
	{
		Set(charValue);
	}
	else if (type == BINARY)
	{
		Set(charValue, (int) numericValue);
	}
	else if (type == POINTER)
	{
		SetPtr(inPtr);
	}
	else
		Clear();
}
Table::ColumnType Table::Cell::EstimateColumnType(void) const
{
	if (c)
	{
		if (i != 0.0f)
			return BINARY;
		else
			return STRING;
	}

	if (ptr)
		return POINTER;
	return NUMERIC;
}
void Table::Cell::Clear(void)
{
	std::free(c);
	c = nullptr;
	isEmpty = true;
}
Table::ColumnDescriptor::ColumnDescriptor()
{
}
Table::ColumnDescriptor::~ColumnDescriptor()
{
}
Table::ColumnDescriptor::ColumnDescriptor(const char cn[_TABLE_MAX_COLUMN_NAME_LENGTH], ColumnType ct)
{
	columnType = ct;
	strcpy_s(columnName, cn);
}
void Table::Row::UpdateCell(unsigned columnIndex, double value)
{
	cells[columnIndex]->Set(value);
}
void Table::Row::UpdateCell(unsigned columnIndex, const char *str)
{
	cells[columnIndex]->Set(str);
}
void Table::Row::UpdateCell(unsigned columnIndex, int byteLength, const char *data)
{
	cells[columnIndex]->Set(data, byteLength);
}
Table::Table()
{
}
Table::~Table()
{
}
Table::Table(const Table &input)
{
	*this = input;
}
Table &Table::operator=(const Table &input)
{
	if (this == &input)
		return *this;

	Clear();

	for (unsigned columnIndex = 0; columnIndex < input.GetColumnCount(); columnIndex++)
		AddColumn(input.ColumnName(columnIndex), input.GetColumnType(columnIndex));

	for (const auto &entry : input.rows)
		AddRow(entry.first, entry.second->cells, false);

	return *this;
}
unsigned Table::AddColumn(const char columnName[_TABLE_MAX_COLUMN_NAME_LENGTH], ColumnType columnType)
{
	if (columnName[0] == 0)
		return (unsigned) -1;

	// Add this column.
	columns.push_back(Table::ColumnDescriptor(columnName, columnType));

	// Extend the rows by one
	for (auto &entry : rows)
		entry.second->cells.push_back(std::make_unique<Cell>());

	return (unsigned) columns.size() - 1;
}
void Table::RemoveColumn(unsigned columnIndex)
{
	if (columnIndex >= columns.size())
		return;

	columns.erase(columns.begin() + columnIndex);

	// Remove this index from each row.
	for (auto &entry : rows)
	{
		std::vector<std::unique_ptr<Cell>> &cells = entry.second->cells;
		cells.erase(cells.begin() + columnIndex);
	}
}
unsigned Table::ColumnIndex(const char *columnName) const
{
	for (std::size_t columnIndex = 0; columnIndex < columns.size(); columnIndex++)
		if (strcmp(columnName, columns[columnIndex].columnName) == 0)
			return (unsigned) columnIndex;
	return (unsigned) -1;
}
unsigned Table::ColumnIndex(char columnName[_TABLE_MAX_COLUMN_NAME_LENGTH]) const
{
	return ColumnIndex((const char *) columnName);
}
char *Table::ColumnName(unsigned index) const
{
	if (index >= columns.size())
		return 0;
	else
		return (char *) columns[index].columnName;
}
Table::ColumnType Table::GetColumnType(unsigned index) const
{
	if (index >= columns.size())
		return (Table::ColumnType) 0;
	else
		return columns[index].columnType;
}
unsigned Table::GetColumnCount(void) const
{
	return (unsigned) columns.size();
}
unsigned Table::GetRowCount(void) const
{
	return (unsigned) rows.size();
}
Table::Row *Table::AddRow(unsigned rowId)
{
	if (rows.count(rowId) != 0)
		return nullptr; // Already exists
	auto newRow = std::make_unique<Row>();
	newRow->cells.reserve(columns.size());
	for (std::size_t columnIndex = 0; columnIndex < columns.size(); columnIndex++)
		newRow->cells.push_back(std::make_unique<Cell>());
	Row *row = newRow.get();
	rows.emplace(rowId, std::move(newRow));
	return row;
}
Table::Row *Table::AddRow(unsigned rowId, const std::vector<Cell> &initialCellValues)
{
	if (rows.count(rowId) != 0)
		return nullptr; // Already exists
	auto newRow = std::make_unique<Row>();
	newRow->cells.reserve(columns.size());
	for (std::size_t columnIndex = 0; columnIndex < columns.size(); columnIndex++)
	{
		if (columnIndex < initialCellValues.size() && initialCellValues[columnIndex].isEmpty == false)
		{
			const Cell &initial = initialCellValues[columnIndex];
			auto cell = std::make_unique<Cell>();
			cell->SetByType(initial.i, initial.c, initial.ptr, columns[columnIndex].columnType);
			newRow->cells.push_back(std::move(cell));
		}
		else
			newRow->cells.push_back(std::make_unique<Cell>());
	}
	Row *row = newRow.get();
	rows.emplace(rowId, std::move(newRow));
	return row;
}

namespace
{
template <class CellList>
void FillCells(Table::Row &newRow, const std::vector<Table::ColumnDescriptor> &columns, const CellList &initialCellValues, bool copyCells)
{
	newRow.cells.reserve(columns.size());
	for (std::size_t columnIndex = 0; columnIndex < columns.size(); columnIndex++)
	{
		const Table::Cell *initial = columnIndex < initialCellValues.size() ? CellOf(initialCellValues[columnIndex]) : nullptr;
		if (initial && initial->isEmpty == false)
		{
			if (copyCells == false)
				newRow.cells.push_back(std::make_unique<Table::Cell>(initial->i, initial->c, initial->ptr, columns[columnIndex].columnType));
			else
				newRow.cells.push_back(std::make_unique<Table::Cell>(*initial));
		}
		else
			newRow.cells.push_back(std::make_unique<Table::Cell>());
	}
}
} // namespace

Table::Row *Table::AddRow(unsigned rowId, const std::vector<Cell *> &initialCellValues, bool copyCells)
{
	if (rows.count(rowId) != 0)
		return nullptr; // Already exists
	auto newRow = std::make_unique<Row>();
	FillCells(*newRow, columns, initialCellValues, copyCells);
	Row *row = newRow.get();
	rows.emplace(rowId, std::move(newRow));
	return row;
}
Table::Row *Table::AddRow(unsigned rowId, const std::vector<std::unique_ptr<Cell>> &initialCellValues, bool copyCells)
{
	if (rows.count(rowId) != 0)
		return nullptr; // Already exists
	auto newRow = std::make_unique<Row>();
	FillCells(*newRow, columns, initialCellValues, copyCells);
	Row *row = newRow.get();
	rows.emplace(rowId, std::move(newRow));
	return row;
}
Table::Row *Table::AddRowColumns(unsigned rowId, Row *row, const std::vector<unsigned> &columnIndices)
{
	// Called on the result table: column k of this table is column columnIndices[k] of the source,
	// with the same type (QueryTable added it that way).
	if (rows.count(rowId) != 0)
		return nullptr;
	auto newRow = std::make_unique<Row>();
	newRow->cells.reserve(columnIndices.size());
	for (std::size_t columnIndex = 0; columnIndex < columnIndices.size(); columnIndex++)
	{
		const Cell &source = *row->cells[columnIndices[columnIndex]];
		if (source.isEmpty == false)
			newRow->cells.push_back(std::make_unique<Cell>(source.i, source.c, source.ptr, columns[columnIndex].columnType));
		else
			newRow->cells.push_back(std::make_unique<Cell>());
	}
	Row *added = newRow.get();
	rows.emplace(rowId, std::move(newRow));
	return added;
}
bool Table::RemoveRow(unsigned rowId)
{
	return rows.erase(rowId) != 0;
}
void Table::RemoveRows(Table *tableContainingRowIDs)
{
	for (const auto &entry : tableContainingRowIDs->GetRows())
		rows.erase(entry.first);
}
bool Table::UpdateCell(unsigned rowId, unsigned columnIndex, int value)
{
	RakAssert(columns[columnIndex].columnType == NUMERIC);

	Row *row = GetRowByID(rowId);
	if (row)
	{
		row->UpdateCell(columnIndex, value);
		return true;
	}
	return false;
}
bool Table::UpdateCell(unsigned rowId, unsigned columnIndex, char *str)
{
	RakAssert(columns[columnIndex].columnType == STRING);

	Row *row = GetRowByID(rowId);
	if (row)
	{
		row->UpdateCell(columnIndex, str);
		return true;
	}
	return false;
}
bool Table::UpdateCell(unsigned rowId, unsigned columnIndex, int byteLength, char *data)
{
	RakAssert(columns[columnIndex].columnType == BINARY);

	Row *row = GetRowByID(rowId);
	if (row)
	{
		row->UpdateCell(columnIndex, byteLength, data);
		return true;
	}
	return false;
}
bool Table::UpdateCellByIndex(unsigned rowIndex, unsigned columnIndex, int value)
{
	RakAssert(columns[columnIndex].columnType == NUMERIC);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->UpdateCell(columnIndex, value);
		return true;
	}
	return false;
}
bool Table::UpdateCellByIndex(unsigned rowIndex, unsigned columnIndex, char *str)
{
	RakAssert(columns[columnIndex].columnType == STRING);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->UpdateCell(columnIndex, str);
		return true;
	}
	return false;
}
bool Table::UpdateCellByIndex(unsigned rowIndex, unsigned columnIndex, int byteLength, char *data)
{
	RakAssert(columns[columnIndex].columnType == BINARY);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->UpdateCell(columnIndex, byteLength, data);
		return true;
	}
	return false;
}
void Table::GetCellValueByIndex(unsigned rowIndex, unsigned columnIndex, int *output)
{
	RakAssert(columns[columnIndex].columnType == NUMERIC);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->cells[columnIndex]->Get(output);
	}
}
void Table::GetCellValueByIndex(unsigned rowIndex, unsigned columnIndex, char *output)
{
	RakAssert(columns[columnIndex].columnType == STRING);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->cells[columnIndex]->Get(output);
	}
}
void Table::GetCellValueByIndex(unsigned rowIndex, unsigned columnIndex, char *output, size_t outputLength)
{
	RakAssert(columns[columnIndex].columnType == STRING);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->cells[columnIndex]->Get(output, outputLength);
	}
}
void Table::GetCellValueByIndex(unsigned rowIndex, unsigned columnIndex, char *output, int *outputLength)
{
	RakAssert(columns[columnIndex].columnType == BINARY);

	Row *row = GetRowByIndex(rowIndex, 0);
	if (row)
	{
		row->cells[columnIndex]->Get(output, outputLength);
	}
}
Table::FilterQuery::FilterQuery()
{
}
Table::FilterQuery::~FilterQuery()
{
}
Table::FilterQuery::FilterQuery(unsigned column, Cell *cell, FilterQueryType op)
    : columnIndex(column), cellValue(cell), operation(op)
{
}
Table::Row *Table::GetRowByID(unsigned rowId) const
{
	const auto found = rows.find(rowId);
	return found == rows.end() ? nullptr : found->second.get();
}

Table::Row *Table::GetRowByIndex(unsigned rowIndex, unsigned *key) const
{
	if (rowIndex >= rows.size())
		return nullptr;
	const auto entry = std::next(rows.begin(), (std::ptrdiff_t) rowIndex);
	if (key)
		*key = entry->first;
	return entry->second.get();
}

void Table::QueryTable(unsigned *columnIndicesSubset, unsigned numColumnSubset, FilterQuery *inclusionFilters, unsigned numInclusionFilters, unsigned *rowIds, unsigned numRowIDs, Table *result)
{
	std::vector<unsigned> columnIndicesToReturn;

	// Clear the result table.
	result->Clear();

	if (columnIndicesSubset && numColumnSubset > 0)
	{
		for (unsigned i = 0; i < numColumnSubset; i++)
		{
			if (columnIndicesSubset[i] < columns.size())
				columnIndicesToReturn.push_back(columnIndicesSubset[i]);
		}
	}
	else
	{
		for (std::size_t i = 0; i < columns.size(); i++)
			columnIndicesToReturn.push_back((unsigned) i);
	}

	if (columnIndicesToReturn.empty())
		return; // No valid columns specified

	for (const unsigned columnIndex : columnIndicesToReturn)
		result->AddColumn(columns[columnIndex].columnName, columns[columnIndex].columnType);

	// Get the column indices of the filter queries.
	std::vector<unsigned> inclusionFilterColumnIndices;
	if (inclusionFilters && numInclusionFilters > 0)
	{
		for (unsigned i = 0; i < numInclusionFilters; i++)
		{
			if (inclusionFilters[i].columnName[0])
				inclusionFilters[i].columnIndex = ColumnIndex(inclusionFilters[i].columnName);
			if (inclusionFilters[i].columnIndex < columns.size())
				inclusionFilterColumnIndices.push_back(inclusionFilters[i].columnIndex);
			else
				inclusionFilterColumnIndices.push_back((unsigned) -1);
		}
	}

	if (rowIds == 0 || numRowIDs == 0)
	{
		// All rows, in ascending row id order
		for (const auto &entry : rows)
			QueryRow(inclusionFilterColumnIndices, columnIndicesToReturn, entry.first, entry.second.get(), inclusionFilters, result);
	}
	else
	{
		// Specific rows
		for (unsigned i = 0; i < numRowIDs; i++)
		{
			Row *row = GetRowByID(rowIds[i]);
			if (row)
				QueryRow(inclusionFilterColumnIndices, columnIndicesToReturn, rowIds[i], row, inclusionFilters, result);
		}
	}
}

void Table::QueryRow(const std::vector<unsigned> &inclusionFilterColumnIndices, const std::vector<unsigned> &columnIndicesToReturn, unsigned key, Table::Row *row, FilterQuery *inclusionFilters, Table *result)
{
	bool pass = false;

	// If no inclusion filters, just add the row
	if (inclusionFilterColumnIndices.empty())
	{
		result->AddRowColumns(key, row, columnIndicesToReturn);
		return;
	}

	// Go through all inclusion filters.  Only add this row if all filters pass.
	for (std::size_t j = 0; j < inclusionFilterColumnIndices.size(); j++)
	{
		const unsigned columnIndex = inclusionFilterColumnIndices[j];
		const FilterQuery &filter = inclusionFilters[j];
		if (columnIndex != (unsigned) -1 && row->cells[columnIndex]->isEmpty == false)
		{
			const Cell &cell = *row->cells[columnIndex];
			const ColumnType columnType = columns[columnIndex].columnType;
			if (columnType == STRING && (cell.c == 0 || filter.cellValue == nullptr || filter.cellValue->c == 0))
				continue;

			// A filter without a value can only test for emptiness.
			static const Cell noValue;
			const Cell &value = filter.cellValue ? *filter.cellValue : noValue;
			if (filter.cellValue == nullptr && filter.operation != QF_IS_EMPTY && filter.operation != QF_NOT_EMPTY)
				pass = false;
			else
				switch (filter.operation)
			{
			case QF_EQUAL:
				switch (columnType)
				{
				case NUMERIC:
					pass = cell.i == value.i;
					break;
				case STRING:
					pass = strcmp(cell.c, value.c) == 0;
					break;
				case BINARY:
					pass = cell.i == value.i && (OwnedLength(cell) == 0 || (cell.c && value.c && memcmp(cell.c, value.c, OwnedLength(cell)) == 0));
					break;
				case POINTER:
					pass = cell.ptr == value.ptr;
					break;
				}
				break;
			case QF_NOT_EQUAL:
				switch (columnType)
				{
				case NUMERIC:
					pass = cell.i != value.i;
					break;
				case STRING:
					pass = strcmp(cell.c, value.c) != 0;
					break;
				case BINARY:
					// Kept from the original implementation: passes when the blobs are equal.
					pass = cell.i == value.i && (OwnedLength(cell) == 0 || (cell.c && value.c && memcmp(cell.c, value.c, OwnedLength(cell)) == 0));
					break;
				case POINTER:
					pass = cell.ptr != value.ptr;
					break;
				}
				break;
			case QF_GREATER_THAN:
				switch (columnType)
				{
				case NUMERIC:
					pass = cell.i > value.i;
					break;
				case STRING:
					pass = strcmp(cell.c, value.c) > 0;
					break;
				case BINARY:
					break;
				case POINTER:
					pass = cell.ptr > value.ptr;
					break;
				}
				break;
			case QF_GREATER_THAN_EQ:
				switch (columnType)
				{
				case NUMERIC:
					pass = cell.i >= value.i;
					break;
				case STRING:
					pass = strcmp(cell.c, value.c) >= 0;
					break;
				case BINARY:
					break;
				case POINTER:
					pass = cell.ptr >= value.ptr;
					break;
				}
				break;
			case QF_LESS_THAN:
				switch (columnType)
				{
				case NUMERIC:
					pass = cell.i < value.i;
					break;
				case STRING:
					pass = strcmp(cell.c, value.c) < 0;
					break;
				case BINARY:
					break;
				case POINTER:
					pass = cell.ptr < value.ptr;
					break;
				}
				break;
			case QF_LESS_THAN_EQ:
				switch (columnType)
				{
				case NUMERIC:
					pass = cell.i <= value.i;
					break;
				case STRING:
					pass = strcmp(cell.c, value.c) <= 0;
					break;
				case BINARY:
					break;
				case POINTER:
					pass = cell.ptr <= value.ptr;
					break;
				}
				break;
			case QF_IS_EMPTY:
				pass = false;
				break;
			case QF_NOT_EMPTY:
				pass = true;
				break;
			default:
				// Filters can come from the network (TableSerializer::DeserializeFilterQuery):
				// an unknown operation fails the filter instead of asserting.
				pass = false;
				break;
			}
		}
		else
		{
			if (filter.operation == QF_IS_EMPTY)
				pass = true;
			else
				pass = false; // No value for this cell
		}

		if (pass == false)
			break;
	}

	if (pass)
		result->AddRowColumns(key, row, columnIndicesToReturn);
}

namespace
{
// Three-way comparison of two rows on the valid sort columns (columnIndices[i] == -1 skips
// sortQueries[i]). Empty cells always go at the end.
int RowSort(const Table::Row &first, const Table::Row &second, const Table::SortQuery *sortQueries, const std::vector<unsigned> &columnIndices, const std::vector<Table::ColumnDescriptor> &columns)
{
	for (std::size_t i = 0; i < columnIndices.size(); i++)
	{
		const unsigned columnIndex = columnIndices[i];
		if (columnIndex == (unsigned) -1)
			continue;

		const Table::Cell &a = *first.cells[columnIndex];
		const Table::Cell &b = *second.cells[columnIndex];
		if (a.isEmpty && b.isEmpty)
			continue;
		if (a.isEmpty)
			return 1;
		if (b.isEmpty)
			return -1;

		int order;
		if (columns[columnIndex].columnType == Table::NUMERIC)
			order = a.i > b.i ? 1 : (a.i < b.i ? -1 : 0);
		else
			order = strcmp(StringOrEmpty(a.c), StringOrEmpty(b.c));
		if (order != 0)
		{
			const bool increasing = sortQueries[i].operation == Table::QS_INCREASING_ORDER;
			return (order > 0) == increasing ? 1 : -1;
		}
	}

	return 0;
}
} // namespace

void Table::SortTable(Table::SortQuery *sortQueries, unsigned numSortQueries, Table::Row **out)
{
	std::vector<unsigned> columnIndices;
	bool anyValid = false;

	for (unsigned i = 0; i < numSortQueries; i++)
	{
		if (sortQueries[i].columnIndex < columns.size() && columns[sortQueries[i].columnIndex].columnType != BINARY)
		{
			columnIndices.push_back(sortQueries[i].columnIndex);
			anyValid = true;
		}
		else
			columnIndices.push_back((unsigned) -1); // Means don't check this column
	}

	std::vector<Row *> sorted;
	sorted.reserve(rows.size());
	for (const auto &entry : rows)
		sorted.push_back(entry.second.get());

	// Stable: rows equal on every sort column stay in ascending row id order.
	if (anyValid)
	{
		std::stable_sort(sorted.begin(), sorted.end(), [&](const Row *first, const Row *second)
		                 { return RowSort(*first, *second, sortQueries, columnIndices, columns) < 0; });
	}

	std::copy(sorted.begin(), sorted.end(), out);
}
void Table::PrintColumnHeaders(char *out, int outLength, char columnDelineator) const
{
	if (outLength <= 0)
		return;
	if (outLength == 1)
	{
		*out = 0;
		return;
	}

	out[0] = 0;
	int len;
	for (std::size_t i = 0; i < columns.size(); i++)
	{
		if (i != 0)
		{
			len = (int) strlen(out);
			if (len < outLength - 1)
				sprintf_s(out + len, outLength - len, "%c", columnDelineator);
			else
				return;
		}

		len = (int) strlen(out);
		if (len < outLength - (int) strlen(columns[i].columnName))
			sprintf_s(out + len, outLength - len, "%s", columns[i].columnName);
		else
			return;
	}
}
void Table::PrintRow(char *out, int outLength, char columnDelineator, bool printDelineatorForBinary, Table::Row *inputRow) const
{
	if (outLength <= 0)
		return;
	if (outLength == 1)
	{
		*out = 0;
		return;
	}

	if (inputRow->cells.size() != columns.size())
	{
		strncpy_s(out, outLength, "Cell width does not match column width.\n", outLength);
		out[outLength - 1] = 0;
		return;
	}

	char buff[512];
	int len;
	out[0] = 0;
	for (std::size_t i = 0; i < columns.size(); i++)
	{
		const Cell &cell = *inputRow->cells[i];
		const bool last = i + 1 == columns.size();
		if (columns[i].columnType == NUMERIC)
		{
			if (cell.isEmpty == false)
			{
				sprintf_s(buff, "%f", cell.i);
				len = (int) strlen(buff);
			}
			else
				len = 0;
			if (!last)
				buff[len++] = columnDelineator;
			buff[len] = 0;
		}
		else if (columns[i].columnType == STRING)
		{
			if (cell.isEmpty == false && cell.c)
			{
				strncpy_s(buff, cell.c, 512 - 2);
				buff[512 - 2] = 0;
				len = (int) strlen(buff);
			}
			else
				len = 0;
			if (!last)
				buff[len++] = columnDelineator;
			buff[len] = 0;
		}
		else if (columns[i].columnType == POINTER)
		{
			if (cell.isEmpty == false && cell.ptr)
			{
				sprintf_s(buff, "%p", cell.ptr);
				len = (int) strlen(buff);
			}
			else
				len = 0;
			if (!last)
				buff[len++] = columnDelineator;
			buff[len] = 0;
		}
		else
		{
			if (printDelineatorForBinary && !last)
			{
				buff[0] = columnDelineator;
				buff[1] = 0;
			}
			else
				buff[0] = 0;
		}

		len = (int) strlen(out);
		if (outLength == len + 1)
			break;
		strncpy_s(out + len, outLength - len, buff, outLength - len);
		out[outLength - 1] = 0;
	}
}

void Table::Clear(void)
{
	rows.clear();
	columns.clear();
}
const std::vector<Table::ColumnDescriptor> &Table::GetColumns(void) const
{
	return columns;
}
const Table::RowMap &Table::GetRows(void) const
{
	return rows;
}
unsigned Table::GetAvailableRowId(void) const
{
	// The first id after the lowest one that is not in use (0 for an empty table).
	bool setKey = false;
	unsigned key = 0;
	for (const auto &entry : rows)
	{
		if (setKey == false)
		{
			key = entry.first + 1;
			setKey = true;
		}
		else
		{
			if (key != entry.first)
				return key;
			key++;
		}
	}
	return key;
}
