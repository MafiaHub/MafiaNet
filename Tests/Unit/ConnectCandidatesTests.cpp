/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <vector>

#include "mafianet/connect_candidates.h"
#include "mafianet/types.h"

using namespace MafiaNet;

namespace
{
SystemAddress A(const char *literal, unsigned short port = 7000)
{
	SystemAddress sa;
	EXPECT_TRUE(sa.FromStringExplicitPort(literal, port));
	return sa;
}

std::vector<SystemAddress> Order(std::vector<SystemAddress> in)
{
	const int count = OrderConnectCandidates(in.data(), (int) in.size());
	in.resize((size_t) count);
	return in;
}

void ExpectSame(const std::vector<SystemAddress> &actual, const std::vector<SystemAddress> &expected)
{
	ASSERT_EQ(actual.size(), expected.size());
	for (size_t i = 0; i < actual.size(); ++i)
		EXPECT_TRUE(actual[i] == expected[i]) << "index " << i << ": " << actual[i].ToString(true) << " != " << expected[i].ToString(true);
}
} // namespace

TEST(ConnectCandidates, InterleavesFamiliesStartingWithFirst)
{
	const SystemAddress v6a = A("2001:db8::a"), v6b = A("2001:db8::b");
	const SystemAddress v4a = A("192.0.2.1"), v4b = A("192.0.2.2");

	ExpectSame(Order({v6a, v6b, v4a, v4b}), {v6a, v4a, v6b, v4b});
	ExpectSame(Order({v4a, v6a, v6b}), {v4a, v6a, v6b});
}

TEST(ConnectCandidates, LeftoverOfLongerFamilyKeepsItsOrder)
{
	const SystemAddress v6a = A("2001:db8::a"), v6b = A("2001:db8::b"), v6c = A("2001:db8::c");
	const SystemAddress v4a = A("192.0.2.1");

	ExpectSame(Order({v6a, v6b, v6c, v4a}), {v6a, v4a, v6b, v6c});
}

TEST(ConnectCandidates, DropsDuplicates)
{
	const SystemAddress v6a = A("2001:db8::a");
	const SystemAddress v4a = A("192.0.2.1");

	ExpectSame(Order({v6a, v6a, v4a, v6a, v4a}), {v6a, v4a});
	// Same host, different port: not a duplicate.
	ExpectSame(Order({v4a, A("192.0.2.1", 7001)}), {v4a, A("192.0.2.1", 7001)});
}

TEST(ConnectCandidates, SingleFamilyUnchanged)
{
	const SystemAddress v4a = A("192.0.2.1"), v4b = A("192.0.2.2"), v4c = A("192.0.2.3");
	ExpectSame(Order({v4c, v4a, v4b}), {v4c, v4a, v4b});

	const SystemAddress v6a = A("2001:db8::a"), v6b = A("2001:db8::b");
	ExpectSame(Order({v6b, v6a}), {v6b, v6a});
}

TEST(ConnectCandidates, EmptyAndSingle)
{
	EXPECT_EQ(OrderConnectCandidates(nullptr, 0), 0);
	SystemAddress one = A("192.0.2.1");
	EXPECT_EQ(OrderConnectCandidates(&one, 1), 1);
	EXPECT_TRUE(one == A("192.0.2.1"));
}
