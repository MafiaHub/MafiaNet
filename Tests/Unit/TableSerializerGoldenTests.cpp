/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/ds_table.h"
#include "mafianet/string_compressor.h"
#include "mafianet/table_serializer.h"

#include <cstring>
#include <string>
#include <vector>

using namespace MafiaNet;
using DataStructures::Table;

namespace
{
const char kUnicodeName[] = "\xC3\xBCn\xC3\xAF"
                            "code";

struct CompressorRef
{
	CompressorRef() { StringCompressor::AddReference(); }
	~CompressorRef() { StringCompressor::RemoveReference(); }
};

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

// Golden bytes captured from MafiaNet 0.26.0 (commit e2c0ba3e) before the
// container/allocator migration, from the table built by BuildTable() below
// (SerializeTable, SerializeRow of row 42, SerializeFilterQuery of
// "score > 0"). NEVER regenerate these arrays to make a test pass: a mismatch
// means the wire format changed.
const unsigned char kGoldenTable[] = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x23, 0x95, 0x80, 0x00,
                                      0x00, 0x00, 0x06, 0x09, 0xB4, 0x0A, 0xC0, 0x40, 0x00, 0x00, 0x03, 0x07,
                                      0xCA, 0x0F, 0x40, 0x40, 0x00, 0x00, 0x01, 0x8D, 0xD1, 0x19, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40,
                                      0x00, 0x00, 0x00, 0x02, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x0B, 0xC0, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
                                      0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                                      0x00, 0x20, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x80, 0x00, 0x00, 0x03, 0x0D, 0x25, 0x30, 0xC0, 0x00, 0x00, 0x00,
                                      0x40, 0x00, 0x00, 0x00, 0x20, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x03, 0x1F,
                                      0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00,
                                      0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x10, 0x11, 0x40, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x05, 0x87,
                                      0xDA, 0x27, 0xDA, 0x52, 0x7D, 0xA2, 0x7D, 0xB6, 0xA2, 0x1C, 0x10, 0x00,
                                      0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x03,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const unsigned kGoldenTableBits = 1505;

const unsigned char kGoldenRow42[] = {0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
                                      0x20, 0x22, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x80, 0x00, 0x00, 0x0B, 0x0F, 0xB4, 0x4F, 0xB4, 0xA4, 0xFB, 0x44, 0xFB,
                                      0x6D, 0x44, 0x38, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x10,
                                      0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00};
const unsigned kGoldenRow42Bits = 489;

const unsigned char kGoldenFilter[] = {0x00, 0x00, 0x00, 0x0C, 0x6E, 0x88, 0xC8, 0x00, 0x00, 0x00, 0x00, 0xC0,
                                       0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const unsigned kGoldenFilterBits = 235;

void ExpectBytes(const unsigned char *golden, size_t goldenSize, unsigned goldenBits, const BitStream &bs)
{
	ASSERT_EQ(goldenBits, (unsigned) bs.GetNumberOfBitsUsed());
	ASSERT_EQ(goldenSize, (size_t) bs.GetNumberOfBytesUsed());
	EXPECT_EQ(0, memcmp(golden, bs.GetData(), goldenSize));
}

bool CellIsString(Table::Cell *cell, const char *expected)
{
	return !cell->isEmpty && cell->c && strcmp(cell->c, expected) == 0;
}
} // namespace

TEST(TableSerializerGolden, SerializeTableMatchesGoldenBytes)
{
	CompressorRef ref;
	Table t;
	BuildTable(t);
	BitStream bs;
	TableSerializer::SerializeTable(&t, &bs);
	ExpectBytes(kGoldenTable, sizeof(kGoldenTable), kGoldenTableBits, bs);
}

TEST(TableSerializerGolden, DeserializeTableRoundTripsRowsColumnsAndUnsetCells)
{
	CompressorRef ref;
	BitStream bs(const_cast<unsigned char *>(kGoldenTable), sizeof(kGoldenTable), true);
	Table t;
	ASSERT_TRUE(TableSerializer::DeserializeTable(&bs, &t));

	const auto &columns = t.GetColumns();
	ASSERT_EQ(4u, columns.Size());
	const char *names[] = {"id", "name", "blob", "score"};
	const Table::ColumnType types[] = {Table::NUMERIC, Table::STRING, Table::BINARY, Table::NUMERIC};
	for (unsigned i = 0; i < 4; i++)
	{
		EXPECT_STREQ(names[i], columns[i].columnName);
		EXPECT_EQ(types[i], columns[i].columnType);
	}

	ASSERT_EQ(3u, t.GetRowCount());

	Table::Row *r7 = t.GetRowByID(7);
	ASSERT_NE(nullptr, r7);
	EXPECT_EQ(7.0, r7->cells[0]->i);
	EXPECT_TRUE(CellIsString(r7->cells[1], "alpha"));
	ASSERT_FALSE(r7->cells[2]->isEmpty);
	ASSERT_EQ(2.0, r7->cells[2]->i);
	EXPECT_EQ(0x00, (unsigned char) r7->cells[2]->c[0]);
	EXPECT_EQ(0xFF, (unsigned char) r7->cells[2]->c[1]);
	ASSERT_FALSE(r7->cells[3]->isEmpty);
	EXPECT_EQ(1.5, r7->cells[3]->i);

	Table::Row *r3 = t.GetRowByID(3);
	ASSERT_NE(nullptr, r3);
	EXPECT_FALSE(r3->cells[0]->isEmpty);
	EXPECT_EQ(3.0, r3->cells[0]->i);
	EXPECT_TRUE(CellIsString(r3->cells[1], ""));
	EXPECT_TRUE(r3->cells[2]->isEmpty);
	EXPECT_TRUE(r3->cells[3]->isEmpty);

	Table::Row *r42 = t.GetRowByID(42);
	ASSERT_NE(nullptr, r42);
	EXPECT_EQ(42.0, r42->cells[0]->i);
	EXPECT_TRUE(CellIsString(r42->cells[1], kUnicodeName));
	ASSERT_FALSE(r42->cells[2]->isEmpty);
	ASSERT_EQ(1.0, r42->cells[2]->i);
	EXPECT_EQ(0x01, (unsigned char) r42->cells[2]->c[0]);
	ASSERT_FALSE(r42->cells[3]->isEmpty);
	EXPECT_EQ(0.0, r42->cells[3]->i);
}

TEST(TableSerializerGolden, SerializeRowMatchesGoldenBytes)
{
	CompressorRef ref;
	Table t;
	BuildTable(t);
	BitStream bs;
	TableSerializer::SerializeRow(t.GetRowByID(42), 42, t.GetColumns(), &bs);
	ExpectBytes(kGoldenRow42, sizeof(kGoldenRow42), kGoldenRow42Bits, bs);
}

TEST(TableSerializerGolden, FilterQueryMatchesGoldenBytes)
{
	CompressorRef ref;
	Table::Cell cell;
	cell.Set(0.0);
	Table::FilterQuery q;
	strcpy(q.columnName, "score");
	q.columnIndex = 3;
	q.cellValue = &cell;
	q.operation = Table::QF_GREATER_THAN;

	BitStream bs;
	TableSerializer::SerializeFilterQuery(&bs, &q);
	ExpectBytes(kGoldenFilter, sizeof(kGoldenFilter), kGoldenFilterBits, bs);
}

// Current behaviour: DeserializeTable() hits RakAssert(0) on a truncated row count (aborts in
// Debug), and DeserializeRow() reads numEntries/cellIndex without checking the Read() result.
// Enabled by the task that fixes it.
TEST(TableSerializerGolden, DISABLED_DeserializeTableRejectsTruncatedInput)
{
	CompressorRef ref;
	for (size_t len = 0; len < sizeof(kGoldenTable); len += 7)
	{
		// Exact-size heap copy so ASan sees any read past the cut.
		std::vector<unsigned char> cut(kGoldenTable, kGoldenTable + len);
		Table t;
		EXPECT_FALSE(TableSerializer::DeserializeTable(cut.data(), (unsigned) len, &t)) << "cut at " << len;
	}
}
