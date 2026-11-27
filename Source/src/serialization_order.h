// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace MafiaNet
{
// Orders one connection's serialization list: drops entries whose rank is 0, then sorts the rest by descending
// rank, keeping the incoming order among equal ranks. rank(entry) runs exactly once per entry, so a rank that costs
// map lookups is not paid again on every comparison. When every remaining rank is equal the order is already final
// and nothing is sorted. \a scratch is reused between calls to avoid an allocation per connection.
template <class T, class RankFn>
void OrderByDescendingRank(std::vector<T *> &list, std::vector<std::pair<uint64_t, T *>> &scratch, RankFn &&rank)
{
	scratch.clear();
	scratch.reserve(list.size());
	bool uniform = true;
	for (T *entry : list)
	{
		const uint64_t value = rank(entry);
		if (!value)
			continue;
		if (!scratch.empty() && scratch.front().first != value)
			uniform = false;
		scratch.emplace_back(value, entry);
	}
	if (!uniform)
		std::stable_sort(scratch.begin(), scratch.end(), [](const auto &a, const auto &b)
		                 { return a.first > b.first; });
	list.clear();
	for (const auto &item : scratch)
		list.push_back(item.second);
}
} // namespace MafiaNet
