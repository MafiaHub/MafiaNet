// Copyright (c) 2026, MafiaHub. Licensed under the MIT-style license in license.txt.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>
#include "mafianet/time.h"

namespace MafiaNet
{
// A drop-tail FIFO shared by every link that references it. Capacity includes
// the packet being serialized. Release its entire buffer at transmission end.
// Propagation and independent path loss happen downstream of this queue.
class SimulatedBottleneck
{
public:
	struct Rate
	{
		TimeUS start;
		uint32_t bytesPerSecond;
	};

	SimulatedBottleneck(uint32_t bufferBytes, std::vector<Rate> schedule)
	    : limit(bufferBytes), rates(std::move(schedule))
	{
		assert(!rates.empty() && rates.front().start == 0);
		for (size_t i = 0; i < rates.size(); ++i)
			assert(rates[i].bytesPerSecond && (!i || rates[i].start > rates[i - 1].start));
	}

	bool Admit(TimeUS now, unsigned bytes, TimeUS &departure)
	{
		QueuedBytes(now);
		if (bytes > limit - queued)
		{
			++drops;
			droppedBytes += bytes;
			return false;
		}
		const auto start = fifo.empty() ? now : fifo.back().departure;
		waitUs.push_back(start - now);
		departure = start;
		uint64_t remaining = uint64_t(bytes) * 1000000;
		for (size_t i = 0; i < rates.size(); ++i)
		{
			const auto speed = rates[i].bytesPerSecond;
			const auto duration = (remaining + speed - 1) / speed;
			if (i + 1 == rates.size() || (departure < rates[i + 1].start && duration <= rates[i + 1].start - departure))
			{
				departure += duration;
				break;
			}
			if (departure < rates[i + 1].start)
			{
				remaining -= (rates[i + 1].start - departure) * speed;
				departure = rates[i + 1].start;
			}
		}
		fifo.push_back({departure, bytes});
		queued += bytes;
		peakBytes = std::max(peakBytes, queued);
		admittedBytes += bytes;
		return true;
	}

	uint32_t QueuedBytes(TimeUS now)
	{
		while (!fifo.empty() && fifo.front().departure <= now)
		{
			queued -= fifo.front().bytes;
			fifo.pop_front();
		}
		return queued;
	}

	uint64_t admittedBytes = 0, droppedBytes = 0, drops = 0, randomDrops = 0;
	uint32_t peakBytes = 0;
	std::vector<TimeUS> waitUs;

private:
	struct Packet
	{
		TimeUS departure;
		unsigned bytes;
	};
	uint32_t limit, queued = 0;
	std::vector<Rate> rates;
	std::deque<Packet> fifo;
};
}
