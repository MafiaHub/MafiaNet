/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "mafianet/get_time.h"
#include "mafianet/locked_queue.h"
#include "mafianet/sleep.h"

using namespace MafiaNet;

namespace
{
constexpr int kProducers = 4;
constexpr int kPushesPerProducer = 10000;

class LockedQueueThreads : public ::testing::Test
{
protected:
	void TearDown() override
	{
		// Join even when an assertion bailed out early, so no thread outlives the fixture.
		for (std::thread &producer : producers)
			if (producer.joinable())
				producer.join();
	}

public:
	// Producer p pushes p*kPushesPerProducer+1 .. (p+1)*kPushesPerProducer, in order.
	void Produce(int p)
	{
		for (int i = 1; i <= kPushesPerProducer; i++)
			queue.Push(p * kPushesPerProducer + i);
	}

protected:
	LockedQueue<int> queue;
	std::vector<std::thread> producers;
};
}

TEST_F(LockedQueueThreads, FourProducersOneConsumerLoseNothing)
{
	// Every value in 1..N is pushed exactly once across all producers.
	for (int p = 0; p < kProducers; p++)
		producers.emplace_back(&LockedQueueThreads::Produce, this, p);

	const std::int64_t expectedCount = (std::int64_t) kProducers * kPushesPerProducer;
	const std::int64_t expectedSum = expectedCount * (expectedCount + 1) / 2;
	std::vector<int> lastSeenPerProducer(kProducers, 0);
	bool perProducerOrderKept = true;
	std::int64_t count = 0;
	std::int64_t sum = 0;

	const MafiaNet::TimeMS start = MafiaNet::GetTimeMS();
	while (MafiaNet::GetTimeMS() - start < 10000 && count < expectedCount)
	{
		bool gotAny = false;
		while (auto value = queue.TryPop())
		{
			gotAny = true;
			count++;
			sum += *value;
			const int producer = (*value - 1) / kPushesPerProducer;
			if (*value <= lastSeenPerProducer[producer])
				perProducerOrderKept = false;
			lastSeenPerProducer[producer] = *value;
		}
		if (!gotAny)
			RakSleep(0);
	}

	EXPECT_EQ(count, expectedCount);
	EXPECT_EQ(sum, expectedSum);
	EXPECT_TRUE(perProducerOrderKept);
	EXPECT_TRUE(queue.IsEmpty());
}
