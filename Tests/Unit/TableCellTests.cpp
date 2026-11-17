/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <new>

#include "mafianet/table.h"

using namespace MafiaNet;

// The copy constructor used to test and free its own uninitialized `c`, and
// left `c` uninitialized when copying a cell without string/binary data, so
// the copy's destructor freed garbage. Constructing into storage filled with
// a non-null pattern makes the old behaviour deterministic instead of relying
// on whatever the stack held.
namespace
{
Table::Cell *CopyIntoDirtyStorage(void *storage, const Table::Cell &source)
{
	std::memset(storage, 0xAB, sizeof(Table::Cell));
	return new (storage) Table::Cell(source);
}
}

TEST(TableCell, CopyOfANumericCellOwnsNoBuffer)
{
	Table::Cell source;
	source.Set(42);

	alignas(Table::Cell) unsigned char storage[sizeof(Table::Cell)];
	Table::Cell *copy = CopyIntoDirtyStorage(storage, source);
	EXPECT_EQ(nullptr, copy->c);
	EXPECT_FALSE(copy->isEmpty);
	EXPECT_EQ(42.0, copy->i);
	copy->~Cell();
}

TEST(TableCell, CopyOfAStringCellOwnsItsOwnBuffer)
{
	Table::Cell source;
	source.Set("hello");

	alignas(Table::Cell) unsigned char storage[sizeof(Table::Cell)];
	Table::Cell *copy = CopyIntoDirtyStorage(storage, source);
	ASSERT_NE(nullptr, copy->c);
	EXPECT_NE(source.c, copy->c);
	EXPECT_STREQ("hello", copy->c);
	copy->~Cell();
	EXPECT_STREQ("hello", source.c);
}

TEST(TableCell, CopyOfAnEmptyCellOwnsNoBuffer)
{
	Table::Cell source;

	alignas(Table::Cell) unsigned char storage[sizeof(Table::Cell)];
	Table::Cell *copy = CopyIntoDirtyStorage(storage, source);
	EXPECT_EQ(nullptr, copy->c);
	EXPECT_TRUE(copy->isEmpty);
	copy->~Cell();
}

TEST(TableCell, StringToStringIsVerbatimNotAFormat)
{
	// Used to go through RakString's printf-style constructor: a peer-supplied "%n" wrote memory.
	const char *text = "%s%n%x 100% %p";
	Table::Cell cell;
	cell.Set(text);
	EXPECT_STREQ(text, cell.ToString(Table::STRING).C_String());
}

TEST(TableCell, SetByTypeTreatsAnOutOfRangeBinaryLengthAsEmpty)
{
	// The double length was cast to int unchecked: undefined for these, and on common targets a
	// saturated INT_MAX that copied 2 GB from a 4-byte buffer.
	char data[4] = {1, 2, 3, 4};
	const double lengths[] = {1e300, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), -5.0};
	for (double length : lengths)
	{
		Table::Cell cell(length, data, nullptr, Table::BINARY);
		EXPECT_FALSE(cell.isEmpty) << length;
		EXPECT_EQ(0.0, cell.i) << length;
		EXPECT_NE(nullptr, cell.c) << length;
	}
	Table::Cell inRange(3.0, data, nullptr, Table::BINARY);
	EXPECT_EQ(3.0, inRange.i);
	EXPECT_EQ(0, std::memcmp(data, inRange.c, 3));
}

TEST(TableCell, BinaryQueryOnACellWithAHugeNumericValueIsWellDefined)
{
	// A binary-column cell without a buffer can hold any double; the query took its length with an
	// unchecked double-to-size_t cast (undefined for infinity; UBSan reports it).
	Table table;
	table.AddColumn("blob", Table::BINARY);
	Table::Row *row = table.AddRow(1);
	ASSERT_NE(nullptr, row);
	row->cells[0]->Set(std::numeric_limits<double>::infinity());

	Table::Cell value;
	value.Set(std::numeric_limits<double>::infinity());
	Table::FilterQuery filter(0, &value, Table::QF_EQUAL);
	Table result;
	table.QueryTable(nullptr, 0, &filter, 1, nullptr, 0, &result);
	EXPECT_TRUE(result.GetRows().empty());
}
