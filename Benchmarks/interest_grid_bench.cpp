// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/interest_grid.h"
#include <benchmark/benchmark.h>

using namespace MafiaNet;

namespace
{
void InterestGridWorkload(benchmark::State &state)
{
	InterestGrid grid;
	grid.Configure({{0, 0}, {1024, 1024}, 16});
	for (NetworkID i = 0; i < 50000; ++i)
		grid.Track(i, {{static_cast<float>(i % 250) * 4, static_cast<float>(i / 250) * 5}});
	std::vector<NetworkID> visible;
	for (unsigned i = 0; i < 20; ++i)
	{
		auto guid = PeerGuid{i + 1};
		grid.SetObserverRadius(guid, {150.0f + i * 15, 150}, 36, 42);
		grid.SetObserverLimits(guid, 100, 16);
		grid.GetVisible(guid, visible);
	}
	const auto before = grid.GetStatistics();
	bool alternate = false;
	for (auto _ : state)
	{
		alternate = !alternate;
		switch (state.range(0))
		{
		case 1:
			grid.SetPosition(49999, {alternate ? 991.f : 992.f, 991});
			break;
		case 2:
			grid.SetPosition(8000, {alternate ? 151.f : 152.f, 150});
			break;
		case 3:
			for (unsigned i = 0; i < 20; ++i)
				grid.SetObserverPosition(PeerGuid{i + 1}, {alternate ? 300.f : 700.f, 150});
			break;
		}
		for (unsigned i = 0; i < 20; ++i)
		{
			grid.GetVisible(PeerGuid{i + 1}, visible);
			benchmark::DoNotOptimize(visible);
		}
	}
	const auto after = grid.GetStatistics();
	state.counters["candidates/tick"] = double(after.candidatesInspected - before.candidatesInspected) / state.iterations();
	state.counters["rebuilds/tick"] = double(after.observersRebuilt - before.observersRebuilt) / state.iterations();
}
BENCHMARK(InterestGridWorkload)->ArgName("motion")->DenseRange(0, 3);

// Categories, entity ranges, look-ahead and dependencies all enabled.
void InterestGridParityFeatures(benchmark::State &state)
{
	InterestGrid grid;
	grid.Configure({{0, 0}, {1024, 1024}, 16});
	for (NetworkID i = 0; i < 50000; ++i)
	{
		SpatialScope scope{{static_cast<float>(i % 250) * 4, static_cast<float>(i / 250) * 5}};
		scope.category = static_cast<uint16_t>(1 + i % 4);
		scope.range = i % 10 == 0 ? 60.0f : 0.0f;
		grid.Track(i + 1, scope);
	}
	grid.SetCategoryLimit(1, 40);
	grid.SetCategoryLimit(2, 40);
	grid.SetCategoryUsesEntityRange(3, true);
	grid.SetLookaheadSeconds(1);
	// Children far away follow parents inside the observers' area.
	for (NetworkID i = 0; i < 250; ++i)
	{
		grid.SetDependency(40001 + i, 7501 + i);
	}
	grid.SetAlwaysVisible(100, true);
	std::vector<NetworkID> visible;
	for (unsigned i = 0; i < 20; ++i)
	{
		auto guid = PeerGuid{i + 1};
		grid.SetObserverRadius(guid, {150.0f + i * 15, 150}, 36, 42);
		grid.SetObserverVelocity(guid, {20, 10});
		grid.SetObserverLimits(guid, 100, 16);
		grid.GetVisible(guid, visible);
	}
	const auto before = grid.GetStatistics();
	bool alternate = false;
	for (auto _ : state)
	{
		alternate = !alternate;
		for (unsigned i = 0; i < 20; ++i)
		{
			grid.SetObserverPosition(PeerGuid{i + 1}, {alternate ? 300.f : 700.f, 150});
		}
		for (unsigned i = 0; i < 20; ++i)
		{
			grid.GetVisible(PeerGuid{i + 1}, visible);
			benchmark::DoNotOptimize(visible);
		}
	}
	const auto after = grid.GetStatistics();
	state.counters["candidates/tick"] = double(after.candidatesInspected - before.candidatesInspected) / state.iterations();
	state.counters["rebuilds/tick"] = double(after.observersRebuilt - before.observersRebuilt) / state.iterations();
	state.counters["depPulls/tick"] = double(after.dependencyPulls - before.dependencyPulls) / state.iterations();
}
BENCHMARK(InterestGridParityFeatures);
}
