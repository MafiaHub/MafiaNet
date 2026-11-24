// Copyright (c) 2026, MafiaHub. MIT license.
#include "FragmentSnapshotStudy.h"
#include <benchmark/benchmark.h>

namespace
{
template <FragmentSnapshotStudy::Policy policy>
void TwoFragmentSnapshot(benchmark::State &state)
{
	for (auto _ : state)
	{
		const auto result = FragmentSnapshotStudy::Run(policy, static_cast<unsigned>(state.range(0)), static_cast<unsigned>(state.range(1)), static_cast<unsigned>(state.range(2)), 12000, FragmentSnapshotStudy::Drop::None, 0, static_cast<unsigned>(state.range(3)));
		if (!result.valid || !result.applied || !result.peakPartialBytes || result.applied > result.publications)
		{
			state.SkipWithError("incomplete snapshot workload");
			return;
		}
		state.counters["p95_age_ms"] = result.p95AgeMs;
		state.counters["p95_send_queue_ms"] = result.p95SendQueueMs;
		state.counters["complete_per_s"] = result.completed / 12.0;
		state.counters["applied_per_s"] = result.applied / 12.0;
		state.counters["published"] = result.publications;
		state.counters["completed"] = result.completed;
		state.counters["applied"] = result.applied;
		state.counters["udp_B"] = static_cast<double>(result.udpBytes);
		state.counters["peak_partial_B"] = static_cast<double>(result.peakPartialBytes);
		state.counters["peak_assembly_B"] = static_cast<double>(result.peakAssemblyBytes);
	}
}
void Cases(benchmark::Benchmark *bench)
{
	bench->ArgsProduct({{30, 60}, {0, 2, 5}, {1, 2, 3, 4, 5}, {0, 12000}})->ArgNames({"hz", "loss_pct", "seed", "warmup_ms"})->Iterations(1);
}
BENCHMARK_TEMPLATE(TwoFragmentSnapshot, FragmentSnapshotStudy::Policy::Promoted)->Name("TwoFragmentSnapshot/Promoted")->Apply(Cases);
BENCHMARK_TEMPLATE(TwoFragmentSnapshot, FragmentSnapshotStudy::Policy::Replaceable)->Name("TwoFragmentSnapshot/Replaceable")->Apply(Cases);
}

namespace
{
template <FragmentSnapshotStudy::Policy policy>
void BurstRecovery(benchmark::State &state)
{
	for (auto _ : state)
	{
		const auto result = FragmentSnapshotStudy::RunBurst(policy, static_cast<unsigned>(state.range(0)), static_cast<unsigned>(state.range(1)), static_cast<unsigned>(state.range(2)), static_cast<unsigned>(state.range(3)));
		if (!result.valid)
		{
			state.SkipWithError("recovery or ordered delivery incomplete");
			return;
		}
		state.counters["first_fresh_ms"] = result.firstFreshMs;
		state.counters["all_fresh_ms"] = result.allFreshMs;
		state.counters["reliable_p99_ms"] = result.reliableP99Ms;
		state.counters["peak_peer_assembly_B"] = static_cast<double>(result.peakAssemblyBytes);
		state.counters["ordered_delivered"] = result.reliableDelivered;
		state.counters["latency_samples"] = result.latencySamples;
		state.counters["outage_drops"] = static_cast<double>(result.outageDrops);
		state.counters["fifo_drops"] = static_cast<double>(result.fifoDrops);
	}
}
void BurstCases(benchmark::Benchmark *bench)
{
	bench->ArgsProduct({{30, 60}, {0, 100, 300, 1000}, {1, 2, 3, 4, 5}, {20, 60}})->ArgNames({"hz", "outage_ms", "seed", "headroom_pct"})->Iterations(1);
}
BENCHMARK_TEMPLATE(BurstRecovery, FragmentSnapshotStudy::Policy::Promoted)->Name("BurstRecovery/Promoted")->Apply(BurstCases);
BENCHMARK_TEMPLATE(BurstRecovery, FragmentSnapshotStudy::Policy::Replaceable)->Name("BurstRecovery/Replaceable")->Apply(BurstCases);
}
