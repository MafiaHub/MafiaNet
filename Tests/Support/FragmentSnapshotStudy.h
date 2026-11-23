// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include <cstddef>
#include <cstdint>

namespace FragmentSnapshotStudy
{
enum class Policy
{
	Promoted,
	Replaceable
};

enum class Drop
{
	None,
	FirstPart,
	LastPart
};

struct Result
{
	bool valid = false, firstCompleted = false;
	unsigned forcedDrops = 0, publications = 0, completed = 0, applied = 0, latest = 0;
	unsigned p95AgeMs = 0, p95SendQueueMs = 0, secondAppliedAtMs = 0;
	std::uint64_t udpBytes = 0;
	std::size_t peakPartialBytes = 0, peakAssemblyBytes = 0, remainingPartialBytes = 0;
};

// A single state stream. Steady runs sample receiver age every simulated millisecond
// after the requested warmup. deadlineMs adds a drain only for the short acceptance cases.
Result Run(Policy policy, unsigned hz, unsigned lossPercent, unsigned seed, unsigned durationMs, Drop drop = Drop::None, unsigned deadlineMs = 0, unsigned warmupMs = 0);
}
