/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  StringCompressor decode behaviour on failure paths. The wire format is
 *  pinned by StringCompressorGoldenTests.cpp.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/string.h"
#include "mafianet/string_compressor.h"

#include <cstring>
#include <string>
#include <type_traits>

using namespace MafiaNet;

namespace
{
const uint8_t kUnregisteredLanguage = 200;

struct CompressorRef
{
	CompressorRef() { StringCompressor::AddReference(); }
	~CompressorRef() { StringCompressor::RemoveReference(); }
};
}

TEST(StringCompressor, DecodeWithUnknownLanguageIdYieldsEmptyRakString)
{
	CompressorRef ref;
	BitStream stream;
	StringCompressor::Instance()->EncodeString("hello", 256, &stream);

	RakString output("stale");
	EXPECT_FALSE(StringCompressor::Instance()->DecodeString(&output, 256, &stream, kUnregisteredLanguage));
	EXPECT_TRUE(output.IsEmpty());
}

TEST(StringCompressor, DecodeWithUnknownLanguageIdTerminatesCharBuffer)
{
	CompressorRef ref;
	BitStream stream;
	StringCompressor::Instance()->EncodeString("hello", 256, &stream);

	char output[16] = {'s', 't', 'a', 'l', 'e', 0};
	EXPECT_FALSE(StringCompressor::Instance()->DecodeString(output, (int) sizeof(output), &stream, kUnregisteredLanguage));
	EXPECT_STREQ("", output);
}

#ifdef _STD_STRING_COMPRESSOR
TEST(StringCompressor, DecodeWithUnknownLanguageIdYieldsEmptyStdString)
{
	CompressorRef ref;
	BitStream stream;
	StringCompressor::Instance()->EncodeString("hello", 256, &stream);

	std::string output = "stale";
	EXPECT_FALSE(StringCompressor::Instance()->DecodeString(&output, 256, &stream, kUnregisteredLanguage));
	EXPECT_TRUE(output.empty());
}
#endif

static_assert(!std::is_copy_constructible_v<StringCompressor>, "StringCompressor owns its trees");
static_assert(!std::is_copy_assignable_v<StringCompressor>, "StringCompressor owns its trees");

TEST(StringCompressor, DecodeIntoSmallBufferTruncatesInsideIt)
{
	CompressorRef ref;
	const char text[] = "a string longer than the destination";
	BitStream stream;
	StringCompressor::Instance()->EncodeString(text, 256, &stream);

	char output[12];
	memset(output, 0x55, sizeof(output));
	ASSERT_TRUE(StringCompressor::Instance()->DecodeString(output, 8, &stream));
	EXPECT_EQ(7u, strlen(output));
	EXPECT_EQ(0, strncmp(output, text, strlen(output)));
	for (size_t i = 8; i < sizeof(output); i++)
		EXPECT_EQ(0x55, (unsigned char) output[i]) << "byte " << i << " written past maxCharsToWrite";
}

TEST(StringCompressor, GeneratedLanguageRoundTripsAndReplacesThePreviousTree)
{
	CompressorRef ref;
	const uint8_t language = 7;
	unsigned char sample[] = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzyx";
	StringCompressor::Instance()->GenerateTreeFromStrings(sample, (unsigned) sizeof(sample) - 1, language);
	BitStream skewed;
	StringCompressor::Instance()->EncodeString("zzzz", 256, &skewed, language);

	unsigned char english[] = "the quick brown fox";
	StringCompressor::Instance()->GenerateTreeFromStrings(english, (unsigned) sizeof(english) - 1, language);
	BitStream replaced;
	StringCompressor::Instance()->EncodeString("zzzz", 256, &replaced, language);
	// 'z' dominated the first sample only, so its code got longer.
	EXPECT_LT(skewed.GetNumberOfBitsUsed(), replaced.GetNumberOfBitsUsed());

	BitStream stream;
	StringCompressor::Instance()->EncodeString("the fox", 256, &stream, language);
	char output[32];
	ASSERT_TRUE(StringCompressor::Instance()->DecodeString(output, (int) sizeof(output), &stream, language));
	EXPECT_STREQ("the fox", output);
}

TEST(StringCompressor, EmptyInputKeepsTheLanguageTree)
{
	CompressorRef ref;
	const uint8_t language = 9;
	unsigned char sample[] = "keep me";
	StringCompressor::Instance()->GenerateTreeFromStrings(sample, (unsigned) sizeof(sample) - 1, language);
	StringCompressor::Instance()->GenerateTreeFromStrings(sample, 0, language);

	BitStream stream;
	StringCompressor::Instance()->EncodeString("keep me", 256, &stream, language);
	char output[32];
	ASSERT_TRUE(StringCompressor::Instance()->DecodeString(output, (int) sizeof(output), &stream, language));
	EXPECT_STREQ("keep me", output);
}
