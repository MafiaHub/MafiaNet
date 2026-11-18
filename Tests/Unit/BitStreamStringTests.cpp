/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"

#include <cstring>
#include <string>
#include <string_view>

using namespace MafiaNet;

TEST(BitStreamString, Writes32BitLengthAndAlignedBytes)
{
	BitStream stream;
	stream.Write(true);
	stream.Write(std::string("a\0b", 3));
	stream.Write(uint8_t{42});

	bool flag;
	uint32_t size;
	ASSERT_TRUE(stream.Read(flag));
	ASSERT_TRUE(flag);
	ASSERT_TRUE(stream.Read(size));
	ASSERT_EQ(size, 3u);
	unsigned char bytes[4];
	ASSERT_TRUE(stream.ReadAlignedBytes(bytes, sizeof(bytes)));
	EXPECT_EQ(std::memcmp(bytes, "a\0b\x2a", sizeof(bytes)), 0);
}

TEST(BitStreamString, WritesUtf8ByteLengthAndPayload)
{
	const std::string text = u8"\u00E9\U0001F642";
	BitStream stream;
	stream.Write(text);

	// U+00E9 takes two bytes and U+1F642 takes four.
	uint32_t length;
	ASSERT_TRUE(stream.Read(length));
	ASSERT_EQ(length, 6u);
	char payload[6];
	ASSERT_TRUE(stream.Read(payload, sizeof(payload)));
	EXPECT_EQ(std::memcmp(payload, "\xc3\xa9\xf0\x9f\x99\x82", sizeof(payload)), 0);
}

TEST(BitStreamString, ReadsUtf8TextFromWireBytes)
{
	unsigned char wire[] = {0x00, 0x00, 0x00, 0x06, 0xc3, 0xa9, 0xf0, 0x9f, 0x99, 0x82};
#ifdef __BITSTREAM_NATIVE_END
	const uint32_t nativeLength = 6;
	std::memcpy(wire, &nativeLength, sizeof(nativeLength));
#endif
	BitStream stream(wire, sizeof(wire), false);
	std::string decoded;
	ASSERT_TRUE(stream.Read(decoded));
	EXPECT_EQ(decoded, u8"\u00E9\U0001F642");
	EXPECT_EQ(stream.GetNumberOfUnreadBits(), 0u);
}

TEST(BitStreamString, CStringAndOwnedTextShareAnEncoding)
{
	BitStream stream;
	const char *input = "100% ready";
	stream.Write(input);
	stream.Write(std::string(input));
	stream.Write("");
	std::string value;
	ASSERT_TRUE(stream.Read(value));
	EXPECT_EQ(value, input);
	char buffer[32];
	ASSERT_TRUE(stream.Read(buffer));
	EXPECT_STREQ(buffer, input);
	ASSERT_TRUE(stream.Read(value));
	EXPECT_TRUE(value.empty());
}

TEST(BitStreamString, RejectsIncompleteOrOversizedFieldsBeforeResizing)
{
	for (uint32_t size : {12u, UINT32_MAX})
	{
		BitStream stream;
		stream.Write(size);
		stream.Write("abc", 3);
		std::string value = "original";
		EXPECT_FALSE(stream.Read(value));
		EXPECT_EQ(value, "original");
	}
}

TEST(BitStreamString, ReadsFullLengthBeyond65535InBothEncodings)
{
	StringCompressor::AddReference();
	struct Release
	{
		~Release() { StringCompressor::RemoveReference(); }
	} release;
	std::string text(70000, 'a');
	text.front() = '\0';
	text[65535] = '\0';
	text.back() = '\xff';

	BitStream stream;
	stream.Write(text);
	stream.WriteCompressed(text);
	stream.WriteCompressed(std::string{});
	stream.WriteCompressed("done");
	const std::string_view slice = std::string_view(text).substr(65534, 3);
	stream.Write(slice);
	stream.WriteCompressed(slice);
	stream.Write(std::string_view{});
	std::string value;
	ASSERT_TRUE(stream.Read(value));
	EXPECT_EQ(value, text);
	value.clear();
	ASSERT_TRUE(stream.ReadCompressed(value));
	EXPECT_EQ(value, text);
	ASSERT_TRUE(stream.ReadCompressed(value));
	EXPECT_TRUE(value.empty());
	char end[5];
	ASSERT_TRUE(stream.ReadCompressed(end));
	EXPECT_STREQ(end, "done");
	unsigned char buffer[4]{};
	ASSERT_TRUE(stream.Read(buffer));
	EXPECT_EQ(std::memcmp(buffer, slice.data(), slice.size()), 0);
	EXPECT_EQ(buffer[3], 0);
	ASSERT_TRUE(stream.ReadCompressed(value));
	EXPECT_EQ(value, slice);
	ASSERT_TRUE(stream.Read(value));
	EXPECT_TRUE(value.empty());
	EXPECT_EQ(stream.GetNumberOfUnreadBits(), 0u);
}
