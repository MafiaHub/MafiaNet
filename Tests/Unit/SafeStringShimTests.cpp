/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under MIT-style license
 *
 * The POSIX vsnprintf_s shim (linux_adapter.cpp / osx_adapter.cpp) used to write the
 * terminator at buffer[sizeOfBuffer] whenever _TRUNCATE truncated the output: one byte past
 * the end. FormatString / FormatStringTS reach it with _TRUNCATE, so any formatted string
 * longer than their buffers corrupted the byte after them. A guard byte after each
 * destination pins the bound. Hermetic: no networking.
 */

#include <gtest/gtest.h>

#include "mafianet/format_string.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <cstdarg>
#include <cstring>
#include <string>

#ifndef _WIN32
// The shims are only compiled on Linux and macOS; MSVC provides the real functions.

namespace
{
int CallVsnprintfS(char *buffer, size_t sizeOfBuffer, size_t count, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	const int result = vsnprintf_s(buffer, sizeOfBuffer, count, format, ap);
	va_end(ap);
	return result;
}
} // namespace

TEST(VsnprintfSShim, TruncateKeepsTheTerminatorInsideTheBuffer)
{
	char block[4 + 1];
	block[4] = 'G';
	CallVsnprintfS(block, 4, _TRUNCATE, "%s", "abcdefgh");
	EXPECT_STREQ(block, "abc");
	EXPECT_EQ(block[4], 'G');
}

TEST(VsnprintfSShim, OutputThatFitsIsWrittenWhole)
{
	char block[8 + 1];
	block[8] = 'G';
	EXPECT_EQ(CallVsnprintfS(block, 8, _TRUNCATE, "%d-%s", 42, "ab"), 5);
	EXPECT_STREQ(block, "42-ab");
	EXPECT_EQ(block[8], 'G');
}

TEST(VsnprintfSShim, ExplicitCountTooSmallIsRejectedWithoutOverflow)
{
	char block[8 + 1];
	block[8] = 'G';
	EXPECT_EQ(CallVsnprintfS(block, 8, 4, "%s", "abcdefgh"), -1);
	EXPECT_STREQ(block, "");
	EXPECT_EQ(block[8], 'G');
}

TEST(VsnprintfSShim, FormatStringTSTruncatesInsideItsBuffer)
{
	// FormatStringTS formats into a caller buffer it assumes is 512 bytes.
	char block[512 + 1];
	block[512] = 'G';
	const std::string longText(600, 'x');
	FormatStringTS(block, "%s", longText.c_str());
	EXPECT_EQ(std::strlen(block), 511u);
	EXPECT_EQ(block[512], 'G');
}

TEST(SprintfSShim, OutputThatDoesNotFitLeavesAnEmptyString)
{
	char block[4 + 1];
	block[4] = 'G';
	sprintf_s(block, 4, "%s", "abcdefgh");
	EXPECT_STREQ(block, "");
	EXPECT_EQ(block[4], 'G');
}
#endif
