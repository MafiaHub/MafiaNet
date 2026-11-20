/*
 *  Copyright (c) 2026, MafiaHub
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */
#ifndef MAFIANET_DATAGRAM_GAP_TRACKER_H
#define MAFIANET_DATAGRAM_GAP_TRACKER_H

#include "mafianet/range_list.h"
#include "mafianet/types.h"

#include <algorithm>
#include <vector>

namespace MafiaNet
{
// Send gaps immediately until a previously missing datagram actually arrives.
// Reliable retransmissions have new datagram numbers, so a late original is
// direct evidence that a gap can resolve without a retransmission.
class DatagramGapTracker
{
public:
	void Receive(uint24_t sequence, unsigned skipped, TimeUS now, TimeUS rtt, RangeList<uint24_t> &ready)
	{
		if (!gaps.empty())
		{
			auto late = std::find_if(gaps.begin(), gaps.end(), [sequence](const Gap &gap)
			                         { return gap.sequence == sequence; });
			if (late != gaps.end())
			{
				reordered = true;
				lastReorder = now;
				gaps.erase(late);
			}
		}
		if (!skipped)
			return;
		const bool defer = reordered && now - lastReorder < reorderMemoryUs;
		const TimeUS delay = defer ? std::clamp<TimeUS>(rtt / 4, 1000, maxDelayUs) : 0;
		for (unsigned offset = skipped; offset; --offset)
		{
			const uint24_t missing = sequence - offset;
			const bool track = gaps.size() < maxTracked;
			if (track)
				gaps.push_back({missing, now, delay});
			if (!track || delay == 0)
				ready.Insert(missing);
		}
	}

	void Update(TimeUS now, RangeList<uint24_t> &ready)
	{
		gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [&](Gap &gap)
		                          {
			const TimeUS age = now - gap.detected;
			if (gap.delay && age >= gap.delay)
			{
				ready.Insert(gap.sequence);
				gap.delay = 0;
			}
			return age >= observationUs; }), gaps.end());
	}

	void Clear()
	{
		gaps.clear();
		reordered = false;
	}

private:
	// Retain reported holes briefly to detect late arrival. Report additional
	// holes immediately when this small per-peer history is full.
	static constexpr size_t maxTracked = 64;
	static constexpr TimeUS maxDelayUs = 50000;
	static constexpr TimeUS observationUs = 2 * maxDelayUs;
	static constexpr TimeUS reorderMemoryUs = 10000000;
	struct Gap
	{
		uint24_t sequence;
		TimeUS detected;
		TimeUS delay;
	};
	std::vector<Gap> gaps;
	bool reordered = false;
	TimeUS lastReorder = 0;
};
}

#endif
