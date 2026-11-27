// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "../../Source/src/serialization_order.h"
#include <algorithm>
#include <map>
#include <random>

using namespace MafiaNet;
namespace
{
using Scratch = std::vector<std::pair<uint64_t, int *>>;

// The comparator ManagedReplication used before ranks were computed once.
std::vector<int *> Reference(std::vector<int *> list, const std::map<int *, uint64_t> &ranks)
{
	auto rank = [&](int *entry)
	{ return ranks.at(entry); };
	list.erase(std::remove_if(list.begin(), list.end(), [&](int *entry)
	                          { return !rank(entry); }),
	           list.end());
	std::stable_sort(list.begin(), list.end(), [&](int *a, int *b)
	                 { return rank(a) > rank(b); });
	return list;
}

struct Fixture
{
	std::vector<int> storage;
	std::vector<int *> list;
	std::map<int *, uint64_t> ranks;
	Fixture(unsigned count, unsigned distinctRanks, unsigned seed)
	{
		storage.resize(count);
		std::mt19937 random(seed);
		for (auto &value : storage)
		{
			list.push_back(&value);
			ranks[&value] = distinctRanks ? random() % distinctRanks : 0;
		}
	}
	std::vector<int *> Order()
	{
		Scratch scratch;
		auto result = list;
		OrderByDescendingRank(result, scratch, [&](int *entry)
		                      { return ranks.at(entry); });
		return result;
	}
};

TEST(SerializationOrder, MatchesTheComparatorOnMixedRanks)
{
	for (unsigned seed = 0; seed < 20; ++seed)
	{
		Fixture f(200, 5, seed);
		EXPECT_EQ(f.Order(), Reference(f.list, f.ranks));
	}
}

TEST(SerializationOrder, KeepsInputOrderWhenAllRanksAreEqual)
{
	Fixture f(100, 0, 1);
	for (auto &entry : f.ranks)
		entry.second = 3;
	EXPECT_EQ(f.Order(), f.list);
	EXPECT_EQ(f.Order(), Reference(f.list, f.ranks));
}

TEST(SerializationOrder, DropsZeroRanksAndKeepsTheRestInOrder)
{
	Fixture f(10, 0, 1);
	for (unsigned i = 0; i < f.list.size(); ++i)
		f.ranks[f.list[i]] = i % 3 == 0 ? 0 : 2;
	const auto ordered = f.Order();
	EXPECT_EQ(ordered, Reference(f.list, f.ranks));
	EXPECT_EQ(ordered.size(), 6u);
	EXPECT_TRUE(std::is_sorted(ordered.begin(), ordered.end()));
}

TEST(SerializationOrder, EmptyAndAllZero)
{
	Fixture f(5, 0, 1);
	EXPECT_TRUE(f.Order().empty());
	f.list.clear();
	EXPECT_TRUE(f.Order().empty());
}

TEST(SerializationOrder, RanksEachEntryOnce)
{
	Fixture f(50, 4, 7);
	Scratch scratch;
	unsigned calls = 0;
	auto result = f.list;
	OrderByDescendingRank(result, scratch, [&](int *entry)
	                      { ++calls; return f.ranks.at(entry); });
	EXPECT_EQ(calls, 50u);
}
} // namespace
