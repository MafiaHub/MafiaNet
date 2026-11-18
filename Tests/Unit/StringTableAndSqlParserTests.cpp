/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  StringTable keeps its strings in a sorted std::vector (the position is the
 *  index on the wire); VariadicSQLParser fills a std::vector<IndexAndType>.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"
#include "mafianet/string_table.h"
#include "mafianet/variadic_sql_parser.h"

#include <cstdarg>
#include <cstring>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_copy_constructible_v<StringTable>, "StringTable is non-copyable");

namespace
{
struct CompressorRef
{
	CompressorRef() { StringCompressor::AddReference(); }
	~CompressorRef() { StringCompressor::RemoveReference(); }
};

void Extract(const std::vector<VariadicSQLParser::IndexAndType> &indices, char ***binary, int **lengths, ...)
{
	va_list args;
	va_start(args, lengths);
	VariadicSQLParser::ExtractArguments(args, indices, binary, lengths);
	va_end(args);
}
} // namespace

TEST(StringTable, RegisteredStringsTravelAsTheirSortedIndex)
{
	StringTable table;
	table.AddString("zeta");
	table.AddString("alpha");
	table.AddString("mid");

	BitStream bs;
	table.EncodeString("mid", 64, &bs);
	bool hasIndex = false;
	StringTableType index = 0;
	ASSERT_TRUE(bs.Read(hasIndex));
	ASSERT_TRUE(bs.Read(index));
	EXPECT_TRUE(hasIndex);
	EXPECT_EQ(1, index); // alpha, mid, zeta

	bs.ResetReadPointer();
	char out[16];
	ASSERT_TRUE(table.DecodeString(out, (int) sizeof(out), &bs));
	EXPECT_STREQ("mid", out);
}

TEST(StringTable, RegistrationOwnsTheCallersText)
{
	StringTable table;
	char name[] = "first registered name";
	table.AddString(name);
	name[0] = 'x';

	BitStream bs;
	bs.Write(true);
	bs.Write(StringTableType{0});
	char out[32];
	ASSERT_TRUE(table.DecodeString(out, sizeof(out), &bs));
	EXPECT_STREQ("first registered name", out);
}

TEST(StringTable, WireIndicesUseUnsignedByteOrderAndPutPrefixesFirst)
{
	StringTable table;
	table.AddString("\x80");
	table.AddString("ab");
	table.AddString("");
	table.AddString("\x7f");
	table.AddString("a");

	StringTableType expected = 0;
	for (const char *name : {"", "a", "ab", "\x7f", "\x80"})
	{
		BitStream bs;
		table.EncodeString(name, 8, &bs);
		ASSERT_EQ(bs.GetNumberOfBitsUsed(), 9u);
		bool hasIndex;
		StringTableType index;
		ASSERT_TRUE(bs.Read(hasIndex));
		ASSERT_TRUE(hasIndex);
		ASSERT_TRUE(bs.Read(index));
		EXPECT_EQ(index, expected++);
	}
}

TEST(StringTable, RegisteredStringIsTerminatedWhenTheOutputIsSmall)
{
	StringTable table;
	table.AddString("alphabet");

	BitStream bs;
	table.EncodeString("alphabet", 32, &bs);
	char out[] = "xxxx!";
	ASSERT_TRUE(table.DecodeString(out, 4, &bs));
	EXPECT_STREQ(out, "alp");
	EXPECT_EQ(out[4], '!');
}

TEST(StringTable, UnregisteredStringFallsBackToTheCompressor)
{
	CompressorRef ref;
	StringTable table;
	table.AddString("known");

	BitStream bs;
	table.EncodeString("unknown", 64, &bs);
	char out[16];
	ASSERT_TRUE(table.DecodeString(out, (int) sizeof(out), &bs));
	EXPECT_STREQ("unknown", out);
}

// The index comes from the network: out of range is rejected, never asserted on.
TEST(StringTable, OutOfRangeIndexIsRejected)
{
	StringTable table;
	table.AddString("only");

	BitStream bs;
	bs.Write(true);
	bs.Write((StringTableType) 1);
	char out[16] = "stale";
	EXPECT_FALSE(table.DecodeString(out, (int) sizeof(out), &bs));
	EXPECT_STREQ("", out);
}

TEST(VariadicSQLParser, TypeMappingIndicesFillTheVector)
{
	std::vector<VariadicSQLParser::IndexAndType> indices = {{99, 99}};
	VariadicSQLParser::GetTypeMappingIndices("a=%i and b=%s or c=%q", indices);
	ASSERT_EQ(2u, indices.size());
	EXPECT_EQ(2u, indices[0].strIndex);
	EXPECT_STREQ("int", VariadicSQLParser::GetTypeMappingAtIndex((int) indices[0].typeMappingIndex));
	EXPECT_EQ(11u, indices[1].strIndex);
	EXPECT_STREQ("text", VariadicSQLParser::GetTypeMappingAtIndex((int) indices[1].typeMappingIndex));

	VariadicSQLParser::GetTypeMappingIndices("no parameters", indices);
	EXPECT_TRUE(indices.empty());
}

TEST(VariadicSQLParser, ArgumentsAreExtractedPerIndexAndFreed)
{
	std::vector<VariadicSQLParser::IndexAndType> indices;
	VariadicSQLParser::GetTypeMappingIndices("%i %s %g", indices);
	ASSERT_EQ(3u, indices.size());

	char **binary = nullptr;
	int *lengths = nullptr;
	Extract(indices, &binary, &lengths, 0x01020304, "text", 2.5);
	ASSERT_NE(nullptr, binary);
	ASSERT_NE(nullptr, lengths);

	EXPECT_EQ((int) sizeof(int), lengths[0]);
	const unsigned char bigEndian[4] = {0x01, 0x02, 0x03, 0x04};
	EXPECT_EQ(0, memcmp(bigEndian, binary[0], 4)); // network order
	EXPECT_EQ(4, lengths[1]);
	EXPECT_STREQ("text", binary[1]);
	EXPECT_EQ((int) sizeof(double), lengths[2]);

	VariadicSQLParser::FreeArguments(indices, binary, lengths);

	// No parameters: nothing allocated, nothing freed.
	std::vector<VariadicSQLParser::IndexAndType> none;
	char **noBinary = nullptr;
	int *noLengths = nullptr;
	Extract(none, &noBinary, &noLengths);
	EXPECT_EQ(nullptr, noBinary);
	VariadicSQLParser::FreeArguments(none, noBinary, noLengths);
}
