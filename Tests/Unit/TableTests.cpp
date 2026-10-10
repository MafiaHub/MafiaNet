/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string.h>
#include <vector>

#include "mafianet/ds_table.h"
#include "mafianet/table_serializer.h"
#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"

using namespace MafiaNet;

/*
Description:
Behavioural tests for DataStructures::Table, written when its row storage moved from the homegrown
B+ tree to std::map (std migration stage 1).

The table had no test coverage at all, so the migration was initially verified only by the library
compiling and the rest of the suite passing -- neither of which touches a single row. These pin the
observable behaviour the B+ tree provided, and in particular the two properties a keyed container
swap can silently break:

- rows iterate in ascending row-id order (GetRows, GetRowByIndex, SortTable, serialization). A
  container with different ordering would still compile and still pass every other test, while
  changing the order rows go on the wire.
- AddRow rejects a duplicate id rather than overwriting, which the B+ tree signalled through the
  return value of its Insert.

Success conditions: each operation behaves as documented, and row order is ascending by id
everywhere it is observable.
*/

namespace
{
	using DataStructures::Table;

	// A table with one numeric column and the given row ids, inserted in the order supplied so the
	// tests can prove the container re-orders them rather than preserving insertion order.
	void BuildTable(Table &table, const std::vector<unsigned> &rowIds)
	{
		table.AddColumn("value", Table::NUMERIC);
		for (size_t i = 0; i < rowIds.size(); ++i)
		{
			Table::Row *row = table.AddRow(rowIds[i]);
			ASSERT_NE(row, nullptr);
			row->UpdateCell(0, (double)(rowIds[i] * 10));
		}
	}

	std::vector<unsigned> KeysInOrder(const Table &table)
	{
		std::vector<unsigned> keys;
		const std::map<unsigned, Table::Row*> &rows = table.GetRows();
		for (std::map<unsigned, Table::Row*>::const_iterator it = rows.begin(); it != rows.end(); ++it)
			keys.push_back(it->first);
		return keys;
	}
} // namespace

TEST(Table, RowsIterateInAscendingIdOrderRegardlessOfInsertionOrder)
{
	Table table;
	std::vector<unsigned> inserted;
	inserted.push_back(50);
	inserted.push_back(10);
	inserted.push_back(30);
	inserted.push_back(20);
	BuildTable(table, inserted);

	const std::vector<unsigned> keys = KeysInOrder(table);
	ASSERT_EQ(keys.size(), (size_t)4);
	EXPECT_EQ(keys[0], 10u);
	EXPECT_EQ(keys[1], 20u);
	EXPECT_EQ(keys[2], 30u);
	EXPECT_EQ(keys[3], 50u);
}

TEST(Table, AddRowRejectsADuplicateIdWithoutReplacingTheRow)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(7);
	BuildTable(table, ids);

	Table::Row *original = table.GetRowByID(7);
	ASSERT_NE(original, nullptr);

	EXPECT_EQ(table.AddRow(7), nullptr) << "a duplicate row id must be refused, not overwrite";
	EXPECT_EQ(table.GetRowByID(7), original) << "the existing row must survive a refused insert";
	EXPECT_EQ(table.GetRowCount(), 1u);
}

// The AddRow overloads that take initial cell values must refuse a duplicate id too. Only the plain
// AddRow(unsigned) overload was covered when the rows moved to std::map, and the others were written
// as rows[rowId] = newRow -- which replaced the stored row, leaked it, and left anything holding a
// pointer to it (Room::tableRow in the Lobby2 rooms container, for one) pointing outside the table.
TEST(Table, AddRowWithInitialCellValuesRefusesADuplicateId)
{
	Table table;
	table.AddColumn("value", Table::NUMERIC);

	DataStructures::List<Table::Cell> initial;
	Table::Cell cell;
	cell.Set(5);
	initial.Insert(cell, _FILE_AND_LINE_);

	Table::Row *first = table.AddRow(1, initial);
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(table.GetRowCount(), 1u);

	Table::Row *second = table.AddRow(1, initial);
	EXPECT_EQ(second, nullptr) << "a duplicate id must be refused, not overwrite the stored row";
	EXPECT_EQ(table.GetRowCount(), 1u);
	EXPECT_EQ(table.GetRowByID(1), first) << "the originally stored row must still be the one held";
}

TEST(Table, AddRowWithCellPointersRefusesADuplicateId)
{
	Table table;
	table.AddColumn("value", Table::NUMERIC);

	DataStructures::List<Table::Cell*> initial;
	Table::Cell *cell = MafiaNet::OP_NEW<Table::Cell>(_FILE_AND_LINE_);
	cell->Set(7);
	initial.Insert(cell, _FILE_AND_LINE_);

	Table::Row *first = table.AddRow(2, initial, true);
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(table.GetRowCount(), 1u);

	Table::Row *second = table.AddRow(2, initial, true);
	EXPECT_EQ(second, nullptr) << "a duplicate id must be refused, not overwrite the stored row";
	EXPECT_EQ(table.GetRowCount(), 1u);
	EXPECT_EQ(table.GetRowByID(2), first);

	MafiaNet::OP_DELETE(cell, _FILE_AND_LINE_);
}

TEST(Table, GetRowByIdFindsPresentRowsAndReportsMissingOnes)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(3);
	ids.push_back(9);
	BuildTable(table, ids);

	ASSERT_NE(table.GetRowByID(3), nullptr);
	ASSERT_NE(table.GetRowByID(9), nullptr);
	EXPECT_EQ(table.GetRowByID(4), nullptr);
	EXPECT_EQ(table.GetRowByID(0), nullptr);
}

TEST(Table, GetRowByIndexWalksRowsInKeyOrderAndReportsTheKey)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(40);
	ids.push_back(10);
	ids.push_back(25);
	BuildTable(table, ids);

	unsigned key = 0;
	Table::Row *row = table.GetRowByIndex(0, &key);
	ASSERT_NE(row, nullptr);
	EXPECT_EQ(key, 10u);

	row = table.GetRowByIndex(1, &key);
	ASSERT_NE(row, nullptr);
	EXPECT_EQ(key, 25u);

	row = table.GetRowByIndex(2, &key);
	ASSERT_NE(row, nullptr);
	EXPECT_EQ(key, 40u);

	// Past the end, and with no key requested.
	EXPECT_EQ(table.GetRowByIndex(3, &key), nullptr);
	EXPECT_NE(table.GetRowByIndex(0, 0), nullptr);
}

TEST(Table, RemoveRowErasesOnlyTheNamedRow)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(1);
	ids.push_back(2);
	ids.push_back(3);
	BuildTable(table, ids);

	EXPECT_TRUE(table.RemoveRow(2));
	EXPECT_EQ(table.GetRowCount(), 2u);
	EXPECT_EQ(table.GetRowByID(2), nullptr);
	EXPECT_NE(table.GetRowByID(1), nullptr);
	EXPECT_NE(table.GetRowByID(3), nullptr);

	EXPECT_FALSE(table.RemoveRow(2)) << "removing an absent row must report failure";
	EXPECT_FALSE(table.RemoveRow(99));
	EXPECT_EQ(table.GetRowCount(), 2u);
}

TEST(Table, RemoveRowsDeletesEveryIdHeldByTheOtherTable)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(1);
	ids.push_back(2);
	ids.push_back(3);
	ids.push_back(4);
	BuildTable(table, ids);

	// The ids to drop are the KEYS of the second table, not its contents.
	Table idHolder;
	std::vector<unsigned> toRemove;
	toRemove.push_back(2);
	toRemove.push_back(4);
	BuildTable(idHolder, toRemove);

	table.RemoveRows(&idHolder);

	EXPECT_EQ(table.GetRowCount(), 2u);
	EXPECT_NE(table.GetRowByID(1), nullptr);
	EXPECT_EQ(table.GetRowByID(2), nullptr);
	EXPECT_NE(table.GetRowByID(3), nullptr);
	EXPECT_EQ(table.GetRowByID(4), nullptr);
}

TEST(Table, AddColumnExtendsEveryExistingRow)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(1);
	ids.push_back(2);
	BuildTable(table, ids);

	ASSERT_EQ(table.GetColumnCount(), 1u);
	ASSERT_EQ(table.GetRowByID(1)->cells.Size(), 1u);

	table.AddColumn("extra", Table::STRING);

	ASSERT_EQ(table.GetColumnCount(), 2u);
	EXPECT_EQ(table.GetRowByID(1)->cells.Size(), 2u) << "an existing row did not gain the new cell";
	EXPECT_EQ(table.GetRowByID(2)->cells.Size(), 2u);
}

TEST(Table, RemoveColumnDropsTheCellFromEveryRow)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(1);
	ids.push_back(2);
	BuildTable(table, ids);
	table.AddColumn("extra", Table::STRING);
	ASSERT_EQ(table.GetRowByID(1)->cells.Size(), 2u);

	table.RemoveColumn(1);

	EXPECT_EQ(table.GetColumnCount(), 1u);
	EXPECT_EQ(table.GetRowByID(1)->cells.Size(), 1u);
	EXPECT_EQ(table.GetRowByID(2)->cells.Size(), 1u);
}

TEST(Table, GetAvailableRowIdReturnsTheLowestUnusedId)
{
	Table empty;
	empty.AddColumn("value", Table::NUMERIC);
	EXPECT_EQ(empty.GetAvailableRowId(), 0u);

	// Contiguous from 0: the next free id is one past the end.
	Table contiguous;
	std::vector<unsigned> ids;
	ids.push_back(0);
	ids.push_back(1);
	ids.push_back(2);
	BuildTable(contiguous, ids);
	EXPECT_EQ(contiguous.GetAvailableRowId(), 3u);

	// With a gap, the gap is returned. Relies on ascending iteration order.
	Table gapped;
	std::vector<unsigned> gappedIds;
	gappedIds.push_back(0);
	gappedIds.push_back(1);
	gappedIds.push_back(3);
	BuildTable(gapped, gappedIds);
	EXPECT_EQ(gapped.GetAvailableRowId(), 2u);
}

TEST(Table, ClearDropsEveryRowAndColumn)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(1);
	ids.push_back(2);
	BuildTable(table, ids);

	table.Clear();

	EXPECT_EQ(table.GetRowCount(), 0u);
	EXPECT_TRUE(table.GetRows().empty());
	EXPECT_EQ(table.GetRowByID(1), nullptr);
}

TEST(Table, AssignmentCopiesRowsInKeyOrder)
{
	Table source;
	std::vector<unsigned> ids;
	ids.push_back(30);
	ids.push_back(10);
	ids.push_back(20);
	BuildTable(source, ids);

	Table copy;
	copy = source;

	EXPECT_EQ(copy.GetRowCount(), 3u);
	const std::vector<unsigned> keys = KeysInOrder(copy);
	ASSERT_EQ(keys.size(), (size_t)3);
	EXPECT_EQ(keys[0], 10u);
	EXPECT_EQ(keys[1], 20u);
	EXPECT_EQ(keys[2], 30u);

	// Deep copy: the rows are the copy's own.
	ASSERT_NE(copy.GetRowByID(10), nullptr);
	EXPECT_NE(copy.GetRowByID(10), source.GetRowByID(10));
}

TEST(Table, QueryTableWithNoRowFilterReturnsEveryRowInKeyOrder)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(5);
	ids.push_back(1);
	ids.push_back(3);
	BuildTable(table, ids);

	Table result;
	table.QueryTable(0, 0, 0, 0, 0, 0, &result);

	EXPECT_EQ(result.GetRowCount(), 3u);
	const std::vector<unsigned> keys = KeysInOrder(result);
	ASSERT_EQ(keys.size(), (size_t)3);
	EXPECT_EQ(keys[0], 1u);
	EXPECT_EQ(keys[1], 3u);
	EXPECT_EQ(keys[2], 5u);
}

TEST(Table, QueryTableWithSpecificRowIdsReturnsOnlyThose)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(1);
	ids.push_back(2);
	ids.push_back(3);
	ids.push_back(4);
	BuildTable(table, ids);

	unsigned wanted[2] = {3, 1};
	Table result;
	table.QueryTable(0, 0, 0, 0, wanted, 2, &result);

	EXPECT_EQ(result.GetRowCount(), 2u);
	EXPECT_NE(result.GetRowByID(1), nullptr);
	EXPECT_NE(result.GetRowByID(3), nullptr);
	EXPECT_EQ(result.GetRowByID(2), nullptr);

	// An id that is not present is skipped rather than producing an empty row.
	unsigned missing[2] = {2, 99};
	Table result2;
	table.QueryTable(0, 0, 0, 0, missing, 2, &result2);
	EXPECT_EQ(result2.GetRowCount(), 1u);
	EXPECT_NE(result2.GetRowByID(2), nullptr);
}

TEST(Table, SortTableWithNoValidQueryReturnsRowsInKeyOrder)
{
	Table table;
	std::vector<unsigned> ids;
	ids.push_back(30);
	ids.push_back(10);
	ids.push_back(20);
	BuildTable(table, ids);

	// A sort query naming a column that does not exist leaves nothing valid to sort on, which is the
	// branch that simply emits the rows as stored.
	Table::SortQuery query;
	query.columnIndex = 99;
	query.operation = Table::QS_INCREASING_ORDER;

	std::vector<Table::Row*> out(3, (Table::Row*)0);
	table.SortTable(&query, 1, &out[0]);

	ASSERT_NE(out[0], nullptr);
	EXPECT_EQ(out[0], table.GetRowByID(10));
	EXPECT_EQ(out[1], table.GetRowByID(20));
	EXPECT_EQ(out[2], table.GetRowByID(30));
}

TEST(Table, SortTableOrdersByTheRequestedColumn)
{
	Table table;
	table.AddColumn("value", Table::NUMERIC);
	// Row ids ascending, values descending, so sorting by value must reverse the stored order and
	// cannot pass by accident.
	Table::Row *first = table.AddRow(1);
	ASSERT_NE(first, nullptr);
	first->UpdateCell(0, 300.0);
	Table::Row *second = table.AddRow(2);
	ASSERT_NE(second, nullptr);
	second->UpdateCell(0, 200.0);
	Table::Row *third = table.AddRow(3);
	ASSERT_NE(third, nullptr);
	third->UpdateCell(0, 100.0);

	Table::SortQuery query;
	query.columnIndex = 0;
	query.operation = Table::QS_INCREASING_ORDER;

	std::vector<Table::Row*> out(3, (Table::Row*)0);
	table.SortTable(&query, 1, &out[0]);

	ASSERT_NE(out[0], nullptr);
	EXPECT_EQ(out[0], third);
	EXPECT_EQ(out[1], second);
	EXPECT_EQ(out[2], first);
}

namespace
{
	// TableSerializer encodes column names through StringCompressor, whose Instance() dereferences a
	// null singleton until someone takes a reference -- normally the RakPeer constructor. No peer
	// exists in a hermetic unit test, so without this the serializer segfaults inside
	// SerializeColumns. Pre-existing behaviour, documented on StringCompressor::Instance().
	class TableSerialization : public ::testing::Test
	{
	public:
		void SetUp() override { StringCompressor::AddReference(); }
		void TearDown() override { StringCompressor::RemoveReference(); }
	};
} // namespace

// The serialized row order is observable by a remote peer, so it is the property most worth pinning
// across a container swap.
TEST_F(TableSerialization, RoundTripPreservesRowIdsAndOrder)
{
	Table source;
	std::vector<unsigned> ids;
	ids.push_back(40);
	ids.push_back(10);
	ids.push_back(25);
	BuildTable(source, ids);

	MafiaNet::BitStream bs;
	TableSerializer::SerializeTable(&source, &bs);

	Table restored;
	ASSERT_TRUE(TableSerializer::DeserializeTable(&bs, &restored));

	EXPECT_EQ(restored.GetRowCount(), source.GetRowCount());
	const std::vector<unsigned> keys = KeysInOrder(restored);
	ASSERT_EQ(keys.size(), (size_t)3);
	EXPECT_EQ(keys[0], 10u);
	EXPECT_EQ(keys[1], 25u);
	EXPECT_EQ(keys[2], 40u);

	// And the cell values travelled with their own rows.
	for (size_t i = 0; i < keys.size(); ++i)
	{
		Table::Row *row = restored.GetRowByID(keys[i]);
		ASSERT_NE(row, nullptr);
		int value = 0;
		row->cells[0]->Get(&value);
		EXPECT_EQ(value, (int)(keys[i] * 10)) << "row " << keys[i] << " lost its value";
	}
}
