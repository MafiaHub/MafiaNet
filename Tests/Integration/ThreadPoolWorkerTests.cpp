/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Worker-thread tests for ThreadPool.
 *
 *  The hermetic cases in Tests/Unit/ThreadPoolQueueTests.cpp cover every queue
 *  operation the public API performs on the calling thread, but one thing is only
 *  observable once a worker runs: inputFunctionQueue and inputQueue are *paired*
 *  arrays, and the worker pops one element from each. If a removal ever took the
 *  element from only one of them, every later job would run with another job's
 *  callback -- silently, with no size mismatch to notice. These tests pair
 *  callbacks and inputs so that mix-up changes the output values.
 *
 *  They live here rather than in the unit suite because they need a real thread
 *  and therefore a deadline poll.
 */

#include <gtest/gtest.h>

#include "mafianet/thread_pool.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"

#include <vector>

namespace
{
	// Each callback tags its result with a different offset, so an output value
	// says which callback produced it and which input it was handed.
	int AddOneHundred(int in, bool *returnOutput, void *) { *returnOutput = true; return in + 100; }
	int AddTwoHundred(int in, bool *returnOutput, void *) { *returnOutput = true; return in + 200; }
	int ProduceNothing(int, bool *returnOutput, void *) { *returnOutput = false; return -1; }

	class ThreadPoolWorker : public ::testing::Test
	{
	protected:
		void TearDown() override
		{
			pool.StopThreads();
			pool.ClearInput();
			pool.ClearOutput();
		}

		// Collects exactly `expected` outputs, or fails on the deadline.
		std::vector<int> DrainOutputs(size_t expected, unsigned int timeoutMS = 10000)
		{
			std::vector<int> got;
			const MafiaNet::TimeMS start = MafiaNet::GetTimeMS();
			while (got.size() < expected && MafiaNet::GetTimeMS() - start < timeoutMS)
			{
				if (pool.HasOutput())
					got.push_back(pool.GetOutput());
				else
					RakSleep(10);
			}
			return got;
		}

		ThreadPool<int, int> pool;
	};
}

// The pairing test. Inputs are queued before any thread starts, so the removal
// happens against a queue nothing is draining; then one worker runs them in order.
TEST_F(ThreadPoolWorker, RemovingAnInputKeepsEveryOtherInputWithItsOwnCallback)
{
	pool.AddInput(AddOneHundred, 10);
	pool.AddInput(AddTwoHundred, 20);
	pool.AddInput(AddOneHundred, 30);
	pool.AddInput(AddTwoHundred, 40);

	pool.RemoveInputAtIndex(1); // drops (AddTwoHundred, 20)

	ASSERT_EQ(pool.InputSize(), 3u);

	ASSERT_TRUE(pool.StartThreads(1, 0));

	const std::vector<int> got = DrainOutputs(3);

	ASSERT_EQ(got.size(), 3u) << "expected three outputs, got " << got.size();

	// A single worker drains the queue head-first, so the order is determined.
	// 230 or 140 here would mean an input was run with its neighbour's callback.
	ASSERT_EQ(got[0], 110) << "AddOneHundred(10)";
	ASSERT_EQ(got[1], 130) << "AddOneHundred(30)";
	ASSERT_EQ(got[2], 240) << "AddTwoHundred(40)";
}

// Removing the head, where an off-by-one in the paired removal is easiest to hide.
TEST_F(ThreadPoolWorker, RemovingTheHeadInputKeepsTheRestPaired)
{
	pool.AddInput(AddOneHundred, 1);
	pool.AddInput(AddTwoHundred, 2);
	pool.AddInput(AddOneHundred, 3);

	pool.RemoveInputAtIndex(0); // drops (AddOneHundred, 1)

	ASSERT_TRUE(pool.StartThreads(1, 0));

	const std::vector<int> got = DrainOutputs(2);

	ASSERT_EQ(got.size(), 2u);
	ASSERT_EQ(got[0], 202) << "AddTwoHundred(2)";
	ASSERT_EQ(got[1], 103) << "AddOneHundred(3)";
}

TEST_F(ThreadPoolWorker, WorkerRunsQueuedJobsInFifoOrder)
{
	for (int i = 1; i <= 8; i++)
		pool.AddInput(AddOneHundred, i);

	ASSERT_TRUE(pool.StartThreads(1, 0));

	const std::vector<int> got = DrainOutputs(8);

	ASSERT_EQ(got.size(), 8u);
	for (int i = 0; i < 8; i++)
		ASSERT_EQ(got[(size_t) i], 100 + i + 1) << "output " << i << " is out of order";
}

// A callback that clears returnOutput must not push anything, while the jobs
// around it still do.
TEST_F(ThreadPoolWorker, CallbacksThatSuppressOutputPushNothing)
{
	pool.AddInput(AddOneHundred, 1);
	pool.AddInput(ProduceNothing, 2);
	pool.AddInput(AddOneHundred, 3);

	ASSERT_TRUE(pool.StartThreads(1, 0));

	const std::vector<int> got = DrainOutputs(2);

	ASSERT_EQ(got.size(), 2u);
	ASSERT_EQ(got[0], 101);
	ASSERT_EQ(got[1], 103);

	// Give the pool a moment to produce a third output it must not produce.
	const MafiaNet::TimeMS start = MafiaNet::GetTimeMS();
	while (MafiaNet::GetTimeMS() - start < 200)
	{
		ASSERT_FALSE(pool.HasOutput()) << "ProduceNothing pushed an output";
		RakSleep(10);
	}
}

// Jobs added while the worker is already running are picked up too, which is the
// path send_to_thread and file_list_transfer actually use.
TEST_F(ThreadPoolWorker, JobsAddedAfterStartupAreStillRun)
{
	ASSERT_TRUE(pool.StartThreads(1, 0));

	pool.AddInput(AddOneHundred, 5);
	pool.AddInput(AddTwoHundred, 6);

	const std::vector<int> got = DrainOutputs(2);

	ASSERT_EQ(got.size(), 2u);
	ASSERT_EQ(got[0], 105);
	ASSERT_EQ(got[1], 206);
}

// StopThreads must leave any unprocessed input in place, so the caller can walk
// the queue and free whatever it owns -- the contract AddInput's comment states
// and send_to_thread's shutdown path relies on.
TEST_F(ThreadPoolWorker, StopThreadsLeavesUnprocessedInputInTheQueue)
{
	ASSERT_TRUE(pool.StartThreads(1, 0));

	// Let the worker reach its idle wait before queueing anything, so the jobs
	// below are not consumed between StartThreads and StopThreads.
	RakSleep(100);
	pool.StopThreads();

	pool.AddInput(AddOneHundred, 7);
	pool.AddInput(AddOneHundred, 8);

	ASSERT_EQ(pool.InputSize(), 2u) << "input queued after StopThreads must stay queued";
	ASSERT_EQ(pool.GetInputAtIndex(0), 7);
	ASSERT_EQ(pool.GetInputAtIndex(1), 8);
	ASSERT_FALSE(pool.HasOutput());
}
