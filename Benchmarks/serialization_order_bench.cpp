// Copyright (c) 2026, MafiaHub. MIT license.
#include "../Source/src/serialization_order.h"
#include <benchmark/benchmark.h>
#include <map>
#include <random>

using namespace MafiaNet;

namespace
{
// A crowd of replicas ordered once per connection per tick, with the same per-replica lookups
// (service age map and interval map) ManagedReplication ranks by. range(0) = replicas = connections,
// range(1) = distinct ranks (1 means no update intervals: every rank equal).
struct Crowd
{
	struct Age
	{
		uint64_t since;
		bool served;
	};
	std::vector<int> storage;
	std::vector<int *> list;
	std::map<int *, Age> ages;
	std::map<int *, unsigned> intervals;
	uint64_t pass = 100;
	Crowd(unsigned count, unsigned distinct) : storage(count)
	{
		std::mt19937 random(1);
		for (auto &value : storage)
		{
			list.push_back(&value);
			ages[&value] = {pass - 1 - (distinct > 1 ? random() % distinct : 0), true};
			if (distinct > 1)
				intervals[&value] = 1;
		}
	}
	uint64_t Rank(int *entry) const
	{
		const auto &age = ages.at(entry);
		const auto interval = intervals.find(entry);
		const auto period = interval == intervals.end() ? 1u : interval->second;
		return (pass - age.since) / period + (age.served ? 0u : 1u);
	}
};

void SerializationOrder_Before(benchmark::State &state)
{
	Crowd crowd(state.range(0), state.range(1));
	for (auto _ : state)
	{
		for (int64_t connection = 0; connection < state.range(0); ++connection)
		{
			auto out = crowd.list;
			out.erase(std::remove_if(out.begin(), out.end(), [&](int *e)
			                         { return !crowd.Rank(e); }),
			          out.end());
			std::stable_sort(out.begin(), out.end(), [&](int *a, int *b)
			                 { return crowd.Rank(a) > crowd.Rank(b); });
			benchmark::DoNotOptimize(out);
		}
	}
}

void SerializationOrder_After(benchmark::State &state)
{
	Crowd crowd(state.range(0), state.range(1));
	std::vector<std::pair<uint64_t, int *>> scratch;
	for (auto _ : state)
	{
		for (int64_t connection = 0; connection < state.range(0); ++connection)
		{
			auto out = crowd.list;
			OrderByDescendingRank(out, scratch, [&](int *e)
			                      { return crowd.Rank(e); });
			benchmark::DoNotOptimize(out);
		}
	}
}
BENCHMARK(SerializationOrder_Before)->ArgsProduct({{300}, {1, 8}})->ArgNames({"replicas_x_connections", "distinct_ranks"});
BENCHMARK(SerializationOrder_After)->ArgsProduct({{300}, {1, 8}})->ArgNames({"replicas_x_connections", "distinct_ranks"});
} // namespace
