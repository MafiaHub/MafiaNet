/*
 * Copyright (c) 2026, MafiaHub
 * Licensed under the MIT-style license in the root directory of this source tree.
 */

#include "mafianet/itoa.h"
#include <gtest/gtest.h>

#include <limits>
#include <string>

TEST(Itoa, UsesLowercaseDigitsAndTerminatesAtTheEndOfTheResult)
{
	char out[] = "xxxx!";
	EXPECT_EQ(out, Itoa(0xabc, out, 16));
	EXPECT_STREQ("abc", out);
	EXPECT_EQ('!', out[4]);
	EXPECT_STREQ("0", Itoa(0, out, 2));
}

TEST(Itoa, RepresentsEverySupportedBase)
{
	char out[3];
	for (int base = 2; base <= 16; ++base)
		EXPECT_STREQ("10", Itoa(base, out, base));
}

TEST(Itoa, UsesAMinusSignOnlyForDecimal)
{
	char out[8];
	EXPECT_STREQ("-15", Itoa(-15, out, 10));
	EXPECT_STREQ("f", Itoa(-15, out, 16));
	EXPECT_STREQ("1111", Itoa(-15, out, 2));
}

TEST(Itoa, FormatsTheIntegerLimits)
{
	const int minimum = std::numeric_limits<int>::min();
	const int maximum = std::numeric_limits<int>::max();
	char out[std::numeric_limits<unsigned int>::digits + 1];
	EXPECT_EQ(std::to_string(minimum), Itoa(minimum, out, 10));
	EXPECT_EQ(std::to_string(maximum), Itoa(maximum, out, 10));
	const std::string binaryMinimum = "1" + std::string(std::numeric_limits<int>::digits, '0');
	EXPECT_EQ(binaryMinimum, Itoa(minimum, out, 2));
}

TEST(Itoa, InvalidBasesReturnAnEmptyString)
{
	for (int base : {1, 17})
	{
		char out[] = "xy";
		EXPECT_EQ(out, Itoa(42, out, base));
		EXPECT_STREQ("", out);
		EXPECT_EQ('y', out[1]);
	}
}
