/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <new>

#include "mafianet/ds_table.h"

using namespace DataStructures;

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
