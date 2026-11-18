/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under the MIT-style license in the root of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/base64_encoder.h"

#include <array>
#include <limits>
#include <utility>

using namespace MafiaNet;

namespace
{
// RFC 4648 section 10.
constexpr std::pair<std::string_view, std::string_view> vectors[] = {
    {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
}

TEST(Base64, EncodesRfc4648Vectors)
{
	for (const auto &[bytes, text] : vectors)
		EXPECT_EQ(EncodeBase64(bytes), text);
	EXPECT_EQ(EncodeBase64(std::string_view("\xfb\xff\0", 3)), "+/8A");
}

TEST(Base64, StandardOutputStaysOnOneLineForLongInputs)
{
	EXPECT_EQ(EncodeBase64(std::string(600, '\0')), std::string(800, 'A'));
}

TEST(Base64, DecodesRfc4648Vectors)
{
	for (const auto &[bytes, text] : vectors)
		EXPECT_EQ(DecodeBase64(text), bytes);
	EXPECT_EQ(DecodeBase64("+/8A"), std::string("\xfb\xff\0", 3));
}

TEST(Base64, RejectsInvalidPaddingAlphabetAndTrailingData)
{
	for (const auto text : {"Zg", "Z===", "Zm9v=", "Zh==", "-_8A", "Zg==extra", " Zg==", "Zg==\r\n"})
		EXPECT_FALSE(DecodeBase64(text)) << text;
	const std::string_view withNul("Zg==\0", 5);
	EXPECT_FALSE(DecodeBase64(withNul));
	EXPECT_FALSE(DecodeBase64(withNul, Base64Format::Mime));
}

TEST(Base64, MimeWrapsAt76ColumnsIncludingPadding)
{
	EXPECT_EQ(EncodeBase64({}, Base64Format::Mime), "\r\n");
	for (size_t length : {55u, 56u, 57u, 58u, 114u, 115u})
	{
		const std::string input(length, '\0');
		const auto plain = EncodeBase64(input);
		std::string expected;
		for (size_t i = 0; i < plain.size(); i += 76)
			expected += plain.substr(i, 76) + "\r\n";
		EXPECT_EQ(EncodeBase64(input, Base64Format::Mime), expected) << length;
		EXPECT_EQ(DecodeBase64(expected, Base64Format::Mime), input) << length;
	}
}

TEST(Base64, MimeAcceptsWhitespaceAndRejectsNonAlphabetBytes)
{
	EXPECT_EQ(DecodeBase64(" \tZ\r\nm9v\r\n", Base64Format::Mime), "foo");
	EXPECT_FALSE(DecodeBase64("Zm9v!", Base64Format::Mime));
}

TEST(Base64, LegacyBufferContainsTerminatedMimeText)
{
	// The input is binary and only the first byte belongs to this field.
	const unsigned char input[] = {'f', 0xff};
	std::array<char, 8> buffer{};
	buffer.back() = 'G';
	EXPECT_EQ(Base64Encoding(input, 1, buffer.data()), 6);
	EXPECT_STREQ(buffer.data(), "Zg==\r\n");
	EXPECT_EQ(buffer.back(), 'G');
	EXPECT_EQ(Base64Encoding(nullptr, 0, buffer.data()), 2);
	EXPECT_STREQ(buffer.data(), "\r\n");
	EXPECT_EQ(Base64Encoding(input, -1, buffer.data()), -1);
	EXPECT_EQ(Base64Encoding(input, std::numeric_limits<int>::max(), buffer.data()), -1);
}
