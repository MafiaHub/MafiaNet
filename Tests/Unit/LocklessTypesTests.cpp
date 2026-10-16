/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/lockless_types.h"

#include <thread>
#include <vector>

using namespace MafiaNet;

TEST(LocklessUint32, DefaultsToZero)
{
	const LocklessUint32_t counter;
	EXPECT_EQ(counter.GetValue(), 0u);
}

TEST(LocklessUint32, IncrementReturnsTheValueAfterTheChange)
{
	LocklessUint32_t counter(5);
	EXPECT_EQ(counter.Increment(), 6u);
	EXPECT_EQ(counter.GetValue(), 6u);
}

TEST(LocklessUint32, DecrementReturnsTheValueAfterTheChange)
{
	LocklessUint32_t counter(5);
	EXPECT_EQ(counter.Decrement(), 4u);
	EXPECT_EQ(counter.GetValue(), 4u);
}

TEST(LocklessUint32, DecrementWrapsBelowZero)
{
	LocklessUint32_t counter;
	EXPECT_EQ(counter.Decrement(), UINT32_MAX);
}

// No update is lost when threads contend; under TSan this also proves the
// counter is race-free.
TEST(LocklessUint32, ConcurrentIncrementsAndDecrementsBalance)
{
	constexpr int kThreads = 8;
	constexpr int kIterations = 10000;
	LocklessUint32_t counter(1000);

	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back([&counter, t]
		{
			for (int i = 0; i < kIterations; ++i)
			{
				if (t % 2 == 0)
					counter.Increment();
				else
					counter.Decrement();
			}
		});
	}
	for (std::thread &thread : threads)
		thread.join();

	EXPECT_EQ(counter.GetValue(), 1000u);
}
