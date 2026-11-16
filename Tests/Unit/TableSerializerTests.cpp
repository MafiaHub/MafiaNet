/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// TableSerializer behaviour beyond the pinned golden bytes (TableSerializerGoldenTests.cpp):
// malformed input from a peer is rejected instead of asserting, and edge-case cells round-trip.

#include <gtest/gtest.h>

#include <cstring>
#include <new>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"
#include "mafianet/table.h"
#include "mafianet/table_serializer.h"

using namespace MafiaNet;

namespace
{
struct CompressorRef
{
	CompressorRef() { StringCompressor::AddReference(); }
	~CompressorRef() { StringCompressor::RemoveReference(); }
};

std::vector<unsigned char> Bytes(const BitStream &bs)
{
	return std::vector<unsigned char>(bs.GetData(), bs.GetData() + bs.GetNumberOfBytesUsed());
}

// Columns of a one-NUMERIC-column table, then the row count.
void WriteOneColumnHeader(BitStream &bs, unsigned rowCount)
{
	Table columns;
	columns.AddColumn("v", Table::NUMERIC);
	TableSerializer::SerializeColumns(&columns, &bs);
	bs.Write(rowCount);
}

void WriteNumericRow(BitStream &bs, unsigned key, unsigned cellIndex, double value)
{
	bs.Write(key);
	bs.Write(1u); // numEntries
	bs.Write(cellIndex);
	bs.Write(false); // isEmpty
	bs.Write(value);
}
} // namespace

TEST(TableSerializer, ZeroLengthBinaryCellRoundTrips)
{
	CompressorRef ref;
	Table t;
	t.AddColumn("blob", Table::BINARY);
	t.AddRow(1)->cells[0]->Set("unused", 0);
	t.AddRow(2)->cells[0]->Set(nullptr, 0);
	t.AddRow(3)->cells[0]->Set("\x07\x08", 2);

	BitStream bs;
	TableSerializer::SerializeTable(&t, &bs);

	Table back;
	ASSERT_TRUE(TableSerializer::DeserializeTable(&bs, &back));
	ASSERT_EQ(3u, back.GetRowCount());
	for (unsigned id : {1u, 2u})
	{
		const Table::Cell &cell = *back.GetRowByID(id)->cells[0];
		EXPECT_FALSE(cell.isEmpty) << id;
		EXPECT_EQ(0.0, cell.i) << id;
	}
	const Table::Cell &blob = *back.GetRowByID(3)->cells[0];
	ASSERT_EQ(2.0, blob.i);
	EXPECT_EQ(0, memcmp("\x07\x08", blob.c, 2));
}

TEST(TableSerializer, ZeroLengthBinaryCellIsALengthZeroBlobOnTheWire)
{
	Table::Cell cell;
	cell.Set("unused", 0);
	BitStream actual;
	actual.Write(true); // Misalign, so the byte alignment of the blob shows.
	TableSerializer::SerializeCell(&actual, &cell, Table::BINARY);

	BitStream expected;
	expected.Write(true);
	expected.Write(false); // isEmpty
	expected.Write(0u);    // binaryLength
	expected.AlignWriteToByteBoundary();
	EXPECT_EQ(expected.GetNumberOfBitsUsed(), actual.GetNumberOfBitsUsed());
	EXPECT_EQ(Bytes(expected), Bytes(actual));
}

TEST(TableSerializer, ThreeArgumentFilterQuerySerializesDeterministically)
{
	CompressorRef ref;
	Table::Cell cell;
	cell.Set(0.0);

	// Construct over garbage: the old constructor left columnName uninitialized and
	// SerializeFilterQuery wrote whatever it held.
	alignas(Table::FilterQuery) unsigned char storage[sizeof(Table::FilterQuery)];
	std::memset(storage, 0xAB, sizeof(storage));
	Table::FilterQuery *dirty = new (storage) Table::FilterQuery(3, &cell, Table::QF_GREATER_THAN);
	EXPECT_EQ(0, dirty->columnName[0]);

	Table::FilterQuery clean;
	clean.columnIndex = 3;
	clean.cellValue = &cell;
	clean.operation = Table::QF_GREATER_THAN;

	BitStream fromDirty, fromClean;
	TableSerializer::SerializeFilterQuery(&fromDirty, dirty);
	TableSerializer::SerializeFilterQuery(&fromClean, &clean);
	dirty->~FilterQuery();
	EXPECT_EQ(fromClean.GetNumberOfBitsUsed(), fromDirty.GetNumberOfBitsUsed());
	EXPECT_EQ(Bytes(fromClean), Bytes(fromDirty));
}

TEST(TableSerializer, DeserializeColumnsRejectsAnUnknownColumnType)
{
	// Used to be accepted, then DeserializeCell hit RakAssert(columnType == BINARY) on the row.
	CompressorRef ref;
	auto deserializeWithColumnType = [](unsigned char columnType)
	{
		BitStream bs;
		bs.Write(1u);
		StringCompressor::Instance()->EncodeString("v", _TABLE_MAX_COLUMN_NAME_LENGTH, &bs);
		bs.Write(columnType);
		bs.Write(0u);
		Table t;
		return TableSerializer::DeserializeTable(&bs, &t);
	};
	EXPECT_TRUE(deserializeWithColumnType((unsigned char) Table::POINTER)); // Control: the encoding is right.
	EXPECT_FALSE(deserializeWithColumnType((unsigned char) (Table::POINTER + 1)));
}

TEST(TableSerializer, DeserializeRowRejectsACellIndexPastTheColumns)
{
	CompressorRef ref;
	BitStream bs;
	WriteOneColumnHeader(bs, 1);
	WriteNumericRow(bs, 5, 1, 2.0);

	Table t;
	EXPECT_FALSE(TableSerializer::DeserializeTable(&bs, &t));
	EXPECT_EQ(nullptr, t.GetRowByID(5));
}

TEST(TableSerializer, DeserializeRowRejectsADuplicateRowId)
{
	CompressorRef ref;
	BitStream bs;
	WriteOneColumnHeader(bs, 2);
	WriteNumericRow(bs, 5, 0, 1.0);
	WriteNumericRow(bs, 5, 0, 2.0);

	Table t;
	EXPECT_FALSE(TableSerializer::DeserializeTable(&bs, &t));
	ASSERT_NE(nullptr, t.GetRowByID(5));
	EXPECT_EQ(1.0, t.GetRowByID(5)->cells[0]->i);
}

TEST(TableSerializer, DeserializeTableRejectsATooLargeRowCount)
{
	CompressorRef ref;
	BitStream bs;
	WriteOneColumnHeader(bs, 100001);
	Table t;
	EXPECT_FALSE(TableSerializer::DeserializeTable(&bs, &t));
}

TEST(TableSerializer, FilterQueryRoundTripsNumericAndStringValues)
{
	CompressorRef ref;
	Table::Cell number, text;
	number.Set(12.5);
	text.Set("needle");
	Table::FilterQuery queries[2];
	strcpy(queries[0].columnName, "score");
	queries[0].cellValue = &number;
	queries[0].operation = Table::QF_LESS_THAN_EQ;
	queries[1].columnIndex = 4;
	queries[1].cellValue = &text;
	queries[1].operation = Table::QF_NOT_EQUAL;

	BitStream bs;
	TableSerializer::SerializeFilterQuery(&bs, &queries[0]);
	TableSerializer::SerializeFilterQuery(&bs, &queries[1]);

	Table::Cell numberBack, textBack;
	Table::FilterQuery back[2];
	back[0].cellValue = &numberBack;
	back[1].cellValue = &textBack;
	ASSERT_TRUE(TableSerializer::DeserializeFilterQuery(&bs, &back[0]));
	ASSERT_TRUE(TableSerializer::DeserializeFilterQuery(&bs, &back[1]));

	EXPECT_STREQ("score", back[0].columnName);
	EXPECT_EQ(Table::QF_LESS_THAN_EQ, back[0].operation);
	EXPECT_FALSE(numberBack.isEmpty);
	EXPECT_EQ(12.5, numberBack.i);
	EXPECT_EQ(nullptr, numberBack.c);

	EXPECT_EQ(4u, back[1].columnIndex);
	EXPECT_EQ(Table::QF_NOT_EQUAL, back[1].operation);
	EXPECT_FALSE(textBack.isEmpty);
	EXPECT_STREQ("needle", textBack.c);
}

TEST(TableSerializer, DeserializeFilterQueryRejectsTruncatedInput)
{
	CompressorRef ref;
	Table::Cell text;
	text.Set("needle");
	Table::FilterQuery query;
	strcpy(query.columnName, "name");
	query.cellValue = &text;
	query.operation = Table::QF_EQUAL;
	BitStream full;
	TableSerializer::SerializeFilterQuery(&full, &query);
	const std::vector<unsigned char> bytes = Bytes(full);

	for (size_t len = 0; len < bytes.size(); len++)
	{
		// Exact-size heap copy so ASan sees any read past the cut.
		std::vector<unsigned char> cut(bytes.begin(), bytes.begin() + (std::ptrdiff_t) len);
		BitStream in(cut.data(), (unsigned) len, false);
		Table::Cell cell;
		Table::FilterQuery back;
		back.cellValue = &cell;
		EXPECT_FALSE(TableSerializer::DeserializeFilterQuery(&in, &back)) << "cut at " << len;
	}
}

TEST(TableSerializer, DeserializeFilterQueryRejectsAnUnknownOperation)
{
	// Written field by field: an out-of-range FilterQueryType cannot be built in C++ without
	// undefined behaviour, but a peer can send any byte. The query used to reach QueryTable and
	// hit RakAssert(0) there.
	CompressorRef ref;
	auto deserializeWithOperation = [](unsigned char op)
	{
		BitStream bs;
		StringCompressor::Instance()->EncodeString("", _TABLE_MAX_COLUMN_NAME_LENGTH, &bs, 0);
		bs.WriteCompressed(0u);
		bs.Write(op);
		bs.Write(false); // isEmpty
		bs.Write(1.0);
		bs.WriteAlignedBytesSafe(nullptr, 0, 10000000);
		bs.Write((void *) nullptr);

		Table::Cell cellBack;
		Table::FilterQuery back;
		back.cellValue = &cellBack;
		return TableSerializer::DeserializeFilterQuery(&bs, &back);
	};
	EXPECT_TRUE(deserializeWithOperation((unsigned char) Table::QF_NOT_EMPTY)); // Control: the encoding is right.
	EXPECT_FALSE(deserializeWithOperation((unsigned char) (Table::QF_NOT_EMPTY + 1)));
	EXPECT_FALSE(deserializeWithOperation(0xFF));
}

TEST(TableSerializer, FilterQueryListRoundTripsAndRejectsMoreThanTheMaximum)
{
	CompressorRef ref;
	Table::Cell cell;
	cell.Set(3.0);
	Table::FilterQuery queries[2];
	queries[0] = Table::FilterQuery(1, &cell, Table::QF_EQUAL);
	queries[1] = queries[0];
	BitStream bs;
	TableSerializer::SerializeFilterQueryList(&bs, queries, 2, 2);

	Table::FilterQuery *back = nullptr;
	unsigned int count = 99;
	ASSERT_TRUE(TableSerializer::DeserializeFilterQueryList(&bs, &back, &count, 2));
	ASSERT_EQ(2u, count);
	ASSERT_NE(nullptr, back);
	for (unsigned i = 0; i < count; i++)
	{
		ASSERT_NE(nullptr, back[i].cellValue);
		EXPECT_EQ(3.0, back[i].cellValue->i);
		EXPECT_EQ(Table::QF_EQUAL, back[i].operation);
	}
	TableSerializer::DeallocateQueryList(back, count);

	// Used to RakAssert(0) (abort in Debug) and then read only maxQueries of them.
	bs.ResetReadPointer();
	back = nullptr;
	count = 99;
	EXPECT_FALSE(TableSerializer::DeserializeFilterQueryList(&bs, &back, &count, 1));
	EXPECT_EQ(nullptr, back);
	EXPECT_EQ(0u, count);
}

TEST(TableSerializer, DeserializeFilterQueryListRejectsTruncatedInput)
{
	CompressorRef ref;
	Table::Cell cell;
	cell.Set("x");
	Table::FilterQuery queries[1];
	queries[0] = Table::FilterQuery(1, &cell, Table::QF_EQUAL);
	BitStream full;
	TableSerializer::SerializeFilterQueryList(&full, queries, 1, 4);
	const std::vector<unsigned char> bytes = Bytes(full);

	for (size_t len = 0; len < bytes.size(); len++)
	{
		std::vector<unsigned char> cut(bytes.begin(), bytes.begin() + (std::ptrdiff_t) len);
		BitStream in(cut.data(), (unsigned) len, false);
		Table::FilterQuery *back = nullptr;
		unsigned int count = 0;
		EXPECT_FALSE(TableSerializer::DeserializeFilterQueryList(&in, &back, &count, 4)) << "cut at " << len;
		// Whatever was allocated is released through the documented path (ASan checks it).
		for (unsigned i = 0; i < count; i++)
			EXPECT_NE(nullptr, back[i].cellValue);
		TableSerializer::DeallocateQueryList(back, count);
	}
}
