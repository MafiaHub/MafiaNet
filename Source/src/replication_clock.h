// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/time.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace MafiaNet
{
// One remote simulation timeline. Sender and receiver timestamps have unrelated origins.
class ReplicationClock
{
public:
	static constexpr TimeUS SampleIntervalUS = 100000;

	void Observe(Time tick, TimeUS sent, TimeUS arrived)
	{
		if (!tick || (lastTick && (tick <= lastTick || sent <= lastSent || arrived < lastArrival)))
			return;
		if (lastTick)
		{
			const double spacing = static_cast<double>(sent - lastSent);
			const double measuredRate = static_cast<double>(tick - lastTick) / spacing;
			const double variation = std::abs(static_cast<double>(arrived - lastArrival) - spacing);
			// Smooth arrival jitter separately from simulation rate. A late packet
			// should increase the playback buffer without slowing the simulation clock.
			arrivalJitterUS += (variation - arrivalJitterUS) / 8.0;
			const double previous = Project(arrived);
			const bool ready = ticksPerUS > 0;
			ticksPerUS = ready ? ticksPerUS + (std::clamp(measuredRate, ticksPerUS / 2, ticksPerUS * 2) - ticksPerUS) / 8.0 : measuredRate;
			const double target = std::max(0.0, static_cast<double>(tick) - 2 * arrivalJitterUS * ticksPerUS);
			// Maintain continuity at arrival. Slew small phase errors over 250 ms,
			// hold if ahead, and reacquire promptly after a long gap or a tick jump.
			const double error = target - previous;
			tickAtArrival = ready && error <= std::max(8.0, ticksPerUS * phaseWindowUS) ? previous : target;
			playbackTicksPerUS = ticksPerUS + std::clamp((target - tickAtArrival) / phaseWindowUS, -ticksPerUS, ticksPerUS / 4);
		}
		lastTick = tick;
		lastSent = sent;
		lastArrival = arrived;
	}
	std::optional<double> RenderTick(TimeUS now, unsigned delayTicks) const
	{
		if (ticksPerUS == 0)
			return std::nullopt;
		return std::max(0.0, Project(now) - delayTicks);
	}

private:
	static constexpr double phaseWindowUS = 250000.0;
	double Project(TimeUS now) const
	{
		const double elapsed = now > lastArrival ? static_cast<double>(now - lastArrival) : 0;
		// Freeze during silence. At low simulation rates allow at least two ticks.
		const double horizon = ticksPerUS > 0 ? std::max(phaseWindowUS, 2.0 / ticksPerUS) : 0;
		return tickAtArrival + std::min(elapsed, horizon) * playbackTicksPerUS;
	}
	Time lastTick = 0;
	TimeUS lastSent = 0, lastArrival = 0;
	double ticksPerUS = 0, playbackTicksPerUS = 0, arrivalJitterUS = 0, tickAtArrival = 0;
};
}
