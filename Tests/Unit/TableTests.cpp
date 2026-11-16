/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <type_traits>
#include <vector>

#include "mafianet/table.h"

using MafiaNet::Table;

// Row owns its cells through unique_ptr and is RAK_DLL_EXPORT: its copy operations are deleted
// explicitly (MSVC dllexport would otherwise try to generate them and fail with C2280).
static_assert(!std::is_copy_constructible_v<Table::Row>, "Table::Row owns its cells");
static_assert(!std::is_copy_assignable_v<Table::Row>, "Table::Row owns its cells");
// Table keeps the deep copy the old implementation's operator= provided.
static_assert(std::is_copy_constructible_v<Table>, "Table copies deeply");
static_assert(std::is_copy_assignable_v<Table>, "Table copies deeply");

namespace
{
const char kUnicodeName[] = "\xC3\xBCn\xC3\xAF"
                            "code";

// Same fixture as TableSerializerGoldenTests.cpp.
void BuildTable(Table &t)
{
	t.AddColumn("id", Table::NUMERIC);
	t.AddColumn("name", Table::STRING);
	t.AddColumn("blob", Table::BINARY);
	t.AddColumn("score", Table::NUMERIC);

	const char blob7[] = {0x00, (char) 0xFF};
	Table::Row *r = t.AddRow(7);
	r->cells[0]->Set(7.0);
	r->cells[1]->Set("alpha");
	r->cells[2]->Set(blob7, 2);
	r->cells[3]->Set(1.5);

	r = t.AddRow(3);
	r->cells[0]->Set(3.0);
	r->cells[1]->Set("");

	const char blob42[] = {0x01};
	r = t.AddRow(42);
	r->cells[0]->Set(42.0);
	r->cells[1]->Set(kUnicodeName);
	r->cells[2]->Set(blob42, 1);
	r->cells[3]->Set(0.0);
}

std::vector<unsigned> RowIds(const Table &t)
{
	std::vector<unsigned> ids;
	for (const auto &entry : t.GetRows())
		ids.push_back(entry.first);
	return ids;
}

std::vector<unsigned> SortedIds(Table &t, Table::SortQuery *queries, unsigned numQueries)
{
	std::vector<Table::Row *> out(t.GetRowCount(), nullptr);
	t.SortTable(queries, numQueries, out.data());
	std::vector<unsigned> ids;
	for (Table::Row *row : out)
	{
		unsigned id = (unsigned) -1;
		for (const auto &entry : t.GetRows())
			if (entry.second.get() == row)
				id = entry.first;
		ids.push_back(id);
	}
	return ids;
}
} // namespace

TEST(Table, RowsIterateInAscendingKeyOrder)
{
	Table t;
	t.AddColumn("v", Table::NUMERIC);
	ASSERT_NE(nullptr, t.AddRow(9));
	ASSERT_NE(nullptr, t.AddRow(2));
	ASSERT_NE(nullptr, t.AddRow(5));

	EXPECT_EQ((std::vector<unsigned>{2, 5, 9}), RowIds(t));
}

TEST(Table, RemoveRowFreesItsCells)
{
	// Leak-checked under LSan (linux-lsan.sh): the row, its cells and their buffers must go.
	Table t;
	BuildTable(t);
	ASSERT_TRUE(t.RemoveRow(7));
	EXPECT_FALSE(t.RemoveRow(7));
	EXPECT_EQ(nullptr, t.GetRowByID(7));
	EXPECT_EQ((std::vector<unsigned>{3, 42}), RowIds(t));
}

TEST(Table, RemoveRowsFreesEveryRowTheOtherTableNames)
{
	Table t;
	BuildTable(t);
	Table ids;
	ids.AddColumn("x", Table::NUMERIC);
	ids.AddRow(3);
	ids.AddRow(42);
	ids.AddRow(1000);

	t.RemoveRows(&ids);
	EXPECT_EQ((std::vector<unsigned>{7}), RowIds(t));
}

TEST(Table, QueryWithFilterAndSortMatchesOldBehavior)
{
	Table t;
	BuildTable(t);

	Table::Cell zero;
	zero.Set(0.0);
	Table::FilterQuery filter;
	strcpy(filter.columnName, "score");
	filter.cellValue = &zero;
	filter.operation = Table::QF_GREATER_THAN;

	Table result;
	t.QueryTable(nullptr, 0, &filter, 1, nullptr, 0, &result);
	ASSERT_EQ(4u, result.GetColumnCount());
	EXPECT_EQ(3u, filter.columnIndex); // Looked up by name.
	EXPECT_EQ((std::vector<unsigned>{7}), RowIds(result));

	Table::SortQuery byScoreDescending{3, Table::QS_DECREASING_ORDER};
	EXPECT_EQ((std::vector<unsigned>{7}), SortedIds(result, &byScoreDescending, 1));

	// The row's values come through, typed by column.
	Table::Row *r7 = result.GetRowByID(7);
	ASSERT_NE(nullptr, r7);
	EXPECT_EQ(7.0, r7->cells[0]->i);
	EXPECT_STREQ("alpha", r7->cells[1]->c);
	ASSERT_EQ(2.0, r7->cells[2]->i);
	EXPECT_EQ(0, memcmp("\x00\xFF", r7->cells[2]->c, 2));
	EXPECT_EQ(1.5, r7->cells[3]->i);
}

TEST(Table, SortOnAllRowsPutsEmptyCellsLastAndRespectsDirection)
{
	Table t;
	BuildTable(t);
	Table::SortQuery byScoreDescending{3, Table::QS_DECREASING_ORDER};
	EXPECT_EQ((std::vector<unsigned>{7, 42, 3}), SortedIds(t, &byScoreDescending, 1));

	Table::SortQuery byScoreIncreasing{3, Table::QS_INCREASING_ORDER};
	EXPECT_EQ((std::vector<unsigned>{42, 7, 3}), SortedIds(t, &byScoreIncreasing, 1));

	Table::SortQuery byNameIncreasing{1, Table::QS_INCREASING_ORDER};
	EXPECT_EQ((std::vector<unsigned>{3, 7, 42}), SortedIds(t, &byNameIncreasing, 1));
}

TEST(Table, SortKeepsTiedRowsInAscendingRowIdOrder)
{
	// The old OrderedList-based sort dropped (and in Debug asserted on) rows that tied on every
	// sort column, leaving the tail of the output array unwritten. Every row is now written.
	Table t;
	t.AddColumn("group", Table::NUMERIC);
	t.AddColumn("name", Table::STRING);
	const struct
	{
		unsigned id;
		int group;
		const char *name;
	} rows[] = {{50, 1, "b"}, {10, 2, "a"}, {30, 1, "b"}, {20, 1, "a"}, {40, 2, "a"}};
	for (const auto &r : rows)
	{
		Table::Row *row = t.AddRow(r.id);
		row->cells[0]->Set(r.group);
		row->cells[1]->Set(r.name);
	}

	Table::SortQuery byGroup{0, Table::QS_INCREASING_ORDER};
	EXPECT_EQ((std::vector<unsigned>{20, 30, 50, 10, 40}), SortedIds(t, &byGroup, 1));

	Table::SortQuery byGroupThenName[] = {{0, Table::QS_DECREASING_ORDER}, {1, Table::QS_INCREASING_ORDER}};
	EXPECT_EQ((std::vector<unsigned>{10, 40, 20, 30, 50}), SortedIds(t, byGroupThenName, 2));
}

TEST(Table, SortWithoutAValidColumnReturnsRowsInKeyOrder)
{
	Table t;
	BuildTable(t);
	Table::SortQuery queries[] = {{2, Table::QS_INCREASING_ORDER}, {99, Table::QS_INCREASING_ORDER}};
	EXPECT_EQ((std::vector<unsigned>{3, 7, 42}), SortedIds(t, queries, 2));
}

TEST(Table, QueryColumnSubsetKeepsEachColumnsType)
{
	// Lobby2's room search asks for the POINTER column only and reads cells[0]->ptr.
	Table t;
	t.AddColumn("name", Table::STRING);
	t.AddColumn("ptr", Table::POINTER);
	int target = 0;
	Table::Row *row = t.AddRow(1);
	row->cells[0]->Set("room");
	row->cells[1]->SetPtr(&target);

	unsigned subset[] = {1, 0, 77};
	Table result;
	t.QueryTable(subset, 3, nullptr, 0, nullptr, 0, &result);
	ASSERT_EQ(2u, result.GetColumnCount());
	EXPECT_EQ(Table::POINTER, result.GetColumnType(0));
	EXPECT_EQ(Table::STRING, result.GetColumnType(1));
	Table::Row *copy = result.GetRowByID(1);
	ASSERT_NE(nullptr, copy);
	EXPECT_EQ(&target, copy->cells[0]->ptr);
	EXPECT_STREQ("room", copy->cells[1]->c);
	EXPECT_NE(row->cells[0]->c, copy->cells[1]->c);
}

TEST(Table, QueryBySpecificRowIdsSkipsMissingIds)
{
	Table t;
	BuildTable(t);
	unsigned ids[] = {42, 5, 3};
	Table result;
	t.QueryTable(nullptr, 0, nullptr, 0, ids, 3, &result);
	EXPECT_EQ((std::vector<unsigned>{3, 42}), RowIds(result));
}

TEST(Table, FilterWithoutAValueOnlyTestsEmptiness)
{
	Table t;
	BuildTable(t);
	Table::FilterQuery isEmpty(3, nullptr, Table::QF_IS_EMPTY);
	Table result;
	t.QueryTable(nullptr, 0, &isEmpty, 1, nullptr, 0, &result);
	EXPECT_EQ((std::vector<unsigned>{3}), RowIds(result));

	Table::FilterQuery notEmpty(3, nullptr, Table::QF_NOT_EMPTY);
	t.QueryTable(nullptr, 0, &notEmpty, 1, nullptr, 0, &result);
	EXPECT_EQ((std::vector<unsigned>{7, 42}), RowIds(result));

	Table::FilterQuery equal(3, nullptr, Table::QF_EQUAL);
	t.QueryTable(nullptr, 0, &equal, 1, nullptr, 0, &result);
	EXPECT_EQ(0u, result.GetRowCount());
}

TEST(Table, GetRowByIndexWalksEveryRowInKeyOrder)
{
	// The B+ tree version returned null past its first leaf page (16 rows).
	Table t;
	t.AddColumn("v", Table::NUMERIC);
	for (unsigned id = 100; id > 0; id--)
		t.AddRow(id * 3);

	for (unsigned index = 0; index < 100; index++)
	{
		unsigned key = 0;
		ASSERT_NE(nullptr, t.GetRowByIndex(index, &key)) << index;
		EXPECT_EQ((index + 1) * 3, key);
	}
	EXPECT_EQ(nullptr, t.GetRowByIndex(100, nullptr));
}

TEST(Table, AddRowWithAnExistingIdReturnsNull)
{
	Table t;
	t.AddColumn("v", Table::NUMERIC);
	Table::Row *first = t.AddRow(4);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(nullptr, t.AddRow(4));
	std::vector<Table::Cell> values(1);
	EXPECT_EQ(nullptr, t.AddRow(4, values));
	std::vector<Table::Cell *> pointers{nullptr};
	EXPECT_EQ(nullptr, t.AddRow(4, pointers, true));
	EXPECT_EQ(nullptr, t.AddRow(4, first->cells, true));
	EXPECT_EQ(first, t.GetRowByID(4));
	EXPECT_EQ(1u, t.GetRowCount());
}

TEST(Table, AddRowFromInitialValuesTypesThemByColumn)
{
	Table t;
	t.AddColumn("n", Table::NUMERIC);
	t.AddColumn("s", Table::STRING);
	t.AddColumn("b", Table::BINARY);
	std::vector<Table::Cell> values(3);
	values[0].Set(5);
	values[1].Set("five");
	values[2].Set("\x01\x02\x03", 3);
	Table::Row *row = t.AddRow(1, values);
	ASSERT_NE(nullptr, row);
	EXPECT_EQ(5.0, row->cells[0]->i);
	EXPECT_STREQ("five", row->cells[1]->c);
	EXPECT_NE(values[1].c, row->cells[1]->c);
	ASSERT_EQ(3.0, row->cells[2]->i);
	EXPECT_EQ(0, memcmp("\x01\x02\x03", row->cells[2]->c, 3));
}

TEST(Table, AddRowCopiesAnotherRowsCells)
{
	// Lobby2 copies a room's row into another table with AddRow(id, row->cells, true).
	Table source;
	BuildTable(source);
	Table target;
	for (unsigned i = 0; i < source.GetColumnCount(); i++)
		target.AddColumn(source.ColumnName(i), source.GetColumnType(i));

	Table::Row *copy = target.AddRow(7, source.GetRowByID(7)->cells, true);
	ASSERT_NE(nullptr, copy);
	EXPECT_STREQ("alpha", copy->cells[1]->c);
	EXPECT_NE(source.GetRowByID(7)->cells[1]->c, copy->cells[1]->c);
	EXPECT_EQ(1.5, copy->cells[3]->i);
}

TEST(Table, CopyIsDeep)
{
	Table original;
	BuildTable(original);

	Table copy(original);
	Table assigned;
	assigned.AddColumn("old", Table::STRING);
	assigned.AddRow(1);
	assigned = original;

	for (const Table *t : {&copy, &assigned})
	{
		ASSERT_EQ(4u, t->GetColumnCount());
		EXPECT_EQ((std::vector<unsigned>{3, 7, 42}), RowIds(*t));
		Table::Row *r7 = t->GetRowByID(7);
		ASSERT_NE(nullptr, r7);
		EXPECT_NE(original.GetRowByID(7), r7);
		EXPECT_NE(original.GetRowByID(7)->cells[1]->c, r7->cells[1]->c);
		EXPECT_STREQ("alpha", r7->cells[1]->c);
		EXPECT_EQ(2.0, r7->cells[2]->i);
		EXPECT_TRUE(t->GetRowByID(3)->cells[3]->isEmpty);
	}

	original.Clear();
	EXPECT_STREQ("alpha", copy.GetRowByID(7)->cells[1]->c);
}

TEST(Table, SelfAssignmentKeepsTheTable)
{
	Table t;
	BuildTable(t);
	Table &alias = t;
	t = alias;
	EXPECT_EQ((std::vector<unsigned>{3, 7, 42}), RowIds(t));
	EXPECT_STREQ("alpha", t.GetRowByID(7)->cells[1]->c);
}

TEST(Table, AddAndRemoveColumnResizeEveryRow)
{
	Table t;
	BuildTable(t);
	EXPECT_EQ((unsigned) -1, t.AddColumn("", Table::NUMERIC));
	EXPECT_EQ(4u, t.AddColumn("extra", Table::STRING));
	for (const auto &entry : t.GetRows())
	{
		ASSERT_EQ(5u, entry.second->cells.size());
		EXPECT_TRUE(entry.second->cells[4]->isEmpty);
	}

	t.RemoveColumn(1);
	EXPECT_EQ(4u, t.GetColumnCount());
	EXPECT_STREQ("blob", t.ColumnName(1));
	EXPECT_EQ(3u, t.ColumnIndex("extra"));
	EXPECT_EQ((unsigned) -1, t.ColumnIndex("name"));
	for (const auto &entry : t.GetRows())
		ASSERT_EQ(4u, entry.second->cells.size());
	EXPECT_EQ(2.0, t.GetRowByID(7)->cells[1]->i);
}

TEST(Table, GetAvailableRowIdReturnsTheFirstGapAfterTheLowestId)
{
	Table t;
	t.AddColumn("v", Table::NUMERIC);
	EXPECT_EQ(0u, t.GetAvailableRowId());
	t.AddRow(5);
	EXPECT_EQ(6u, t.GetAvailableRowId());
	t.AddRow(6);
	t.AddRow(8);
	EXPECT_EQ(7u, t.GetAvailableRowId());
	t.AddRow(7);
	EXPECT_EQ(9u, t.GetAvailableRowId());
}

TEST(Table, PrintRowWritesEachColumn)
{
	Table t;
	BuildTable(t);
	char out[256];
	t.PrintRow(out, sizeof(out), ',', true, t.GetRowByID(7));
	EXPECT_STREQ("7.000000,alpha,,1.500000", out);
	t.PrintColumnHeaders(out, sizeof(out), '|');
	EXPECT_STREQ("id|name|blob|score", out);

	Table trailingBinary;
	trailingBinary.AddColumn("b", Table::BINARY);
	trailingBinary.AddRow(1)->cells[0]->Set("\x01", 1);
	trailingBinary.PrintRow(out, sizeof(out), ',', true, trailingBinary.GetRowByID(1));
	EXPECT_STREQ("", out);
}

TEST(TableCell, SetFromItsOwnBufferIsSafe)
{
	Table::Cell cell;
	cell.Set("hello");
	cell.Set(cell.c);
	EXPECT_STREQ("hello", cell.c);
	cell.Set(cell.c, 3);
	ASSERT_EQ(3.0, cell.i);
	EXPECT_EQ(0, memcmp("hel", cell.c, 3));
}

TEST(TableCell, BinaryBufferIsZeroTerminatedPastItsLength)
{
	// A peer can send a STRING filter value as unterminated bytes; strcmp must stop at the cell.
	Table::Cell cell;
	cell.Set("abc", 3);
	ASSERT_EQ(3.0, cell.i);
	EXPECT_EQ(0, cell.c[3]);
	Table::Cell copy(cell);
	EXPECT_EQ(0, copy.c[3]);
}

TEST(TableCell, ZeroLengthBlobIsANonEmptyBinaryCell)
{
	Table::Cell cell;
	cell.Set("ignored", 0);
	EXPECT_FALSE(cell.isEmpty);
	EXPECT_EQ(0.0, cell.i);
	ASSERT_NE(nullptr, cell.c);
	int length = -1;
	char out[1];
	cell.Get(out, &length);
	EXPECT_EQ(0, length);

	cell.Set(nullptr, 5);
	EXPECT_FALSE(cell.isEmpty);
	EXPECT_EQ(0.0, cell.i);
	EXPECT_EQ(nullptr, cell.c);
}

TEST(TableCell, ClearFreesTheBufferAndEmptiesTheCell)
{
	Table::Cell cell;
	cell.Set("text");
	cell.Clear();
	EXPECT_TRUE(cell.isEmpty);
	EXPECT_EQ(nullptr, cell.c);
	cell.SetPtr(&cell);
	EXPECT_EQ(Table::POINTER, cell.EstimateColumnType());
}
