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

#include <string>

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
