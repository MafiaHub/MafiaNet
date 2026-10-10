/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/thread_pool.h"

#include <deque>

// Pins ThreadPool's queue semantics before its three protected queues move from
// DataStructures::Queue to std::deque (issue #60):
//
//   inputFunctionQueue / inputQueue   paired arrays, FIFO, shift-remove by index
//   outputQueue                       FIFO, shift-remove by index
//
// All hermetic: no threads are started, so every queue operation here is the
// synchronous one the public API performs on the calling thread. The pairing
// between inputFunctionQueue and inputQueue is only observable once a worker
// thread drains them, so that lives in Tests/Integration/ThreadPoolWorkerTests.cpp.

namespace
{
	int AddOneHundred(int in, bool *returnOutput, void *) { *returnOutput = true; return in + 100; }
	int AddTwoHundred(int in, bool *returnOutput, void *) { *returnOutput = true; return in + 200; }

	typedef ThreadPool<int, int> IntPool;
}

// --- input queue -----------------------------------------------------------

TEST(ThreadPoolQueues, AddInputAppendsInFifoOrder)
{
	IntPool pool;

	pool.AddInput(AddOneHundred, 10);
	pool.AddInput(AddOneHundred, 20);
	pool.AddInput(AddOneHundred, 30);

	ASSERT_EQ(pool.InputSize(), 3u);
	ASSERT_EQ(pool.GetInputAtIndex(0), 10);
	ASSERT_EQ(pool.GetInputAtIndex(1), 20);
	ASSERT_EQ(pool.GetInputAtIndex(2), 30);
}

// DataStructures::Queue::RemoveAtIndex shifts the later elements down -- it is not
// the swap-remove that DataStructures::List::RemoveAtIndexFast performs. Removing
// from the middle of a four-element queue is what tells the two apart.
TEST(ThreadPoolQueues, RemoveInputAtIndexShiftsLaterInputsDown)
{
	IntPool pool;

	pool.AddInput(AddOneHundred, 10);
	pool.AddInput(AddOneHundred, 20);
	pool.AddInput(AddOneHundred, 30);
	pool.AddInput(AddOneHundred, 40);

	pool.RemoveInputAtIndex(1);

	ASSERT_EQ(pool.InputSize(), 3u);
	ASSERT_EQ(pool.GetInputAtIndex(0), 10);
	ASSERT_EQ(pool.GetInputAtIndex(1), 30) << "a swap-remove would put 40 here";
	ASSERT_EQ(pool.GetInputAtIndex(2), 40);
}

TEST(ThreadPoolQueues, RemoveInputAtTheHeadAndAtTheTail)
{
	IntPool pool;

	for (int i = 1; i <= 4; i++)
		pool.AddInput(AddOneHundred, i * 10);

	pool.RemoveInputAtIndex(0);
	ASSERT_EQ(pool.InputSize(), 3u);
	ASSERT_EQ(pool.GetInputAtIndex(0), 20);

	pool.RemoveInputAtIndex(pool.InputSize() - 1);
	ASSERT_EQ(pool.InputSize(), 2u);
	ASSERT_EQ(pool.GetInputAtIndex(0), 20);
	ASSERT_EQ(pool.GetInputAtIndex(1), 30);
}

TEST(ThreadPoolQueues, HasInputTracksTheInputQueue)
{
	IntPool pool;

	ASSERT_FALSE(pool.HasInput());
	ASSERT_FALSE(pool.HasInputFast());

	pool.AddInput(AddOneHundred, 1);
	ASSERT_TRUE(pool.HasInput());
	ASSERT_TRUE(pool.HasInputFast());

	pool.RemoveInputAtIndex(0);
	ASSERT_FALSE(pool.HasInput());
	ASSERT_FALSE(pool.HasInputFast());
}

// --- output queue ----------------------------------------------------------

TEST(ThreadPoolQueues, AddOutputAppendsAndGetOutputPopsTheHead)
{
	IntPool pool;

	pool.AddOutput(1);
	pool.AddOutput(2);
	pool.AddOutput(3);

	ASSERT_EQ(pool.OutputSize(), 3u);
	ASSERT_EQ(pool.GetOutputAtIndex(0), 1);
	ASSERT_EQ(pool.GetOutputAtIndex(2), 3);

	ASSERT_EQ(pool.GetOutput(), 1) << "GetOutput must take the oldest output, not the newest";
	ASSERT_EQ(pool.GetOutput(), 2);
	ASSERT_EQ(pool.GetOutput(), 3);
	ASSERT_EQ(pool.OutputSize(), 0u);
}

TEST(ThreadPoolQueues, RemoveOutputAtIndexShiftsLaterOutputsDown)
{
	IntPool pool;

	for (int i = 1; i <= 4; i++)
		pool.AddOutput(i);

	pool.RemoveOutputAtIndex(1);

	ASSERT_EQ(pool.OutputSize(), 3u);
	ASSERT_EQ(pool.GetOutputAtIndex(0), 1);
	ASSERT_EQ(pool.GetOutputAtIndex(1), 3) << "a swap-remove would put 4 here";
	ASSERT_EQ(pool.GetOutputAtIndex(2), 4);
}

TEST(ThreadPoolQueues, HasOutputTracksTheOutputQueue)
{
	IntPool pool;

	ASSERT_FALSE(pool.HasOutput());
	ASSERT_FALSE(pool.HasOutputFast());

	pool.AddOutput(7);
	ASSERT_TRUE(pool.HasOutput());
	ASSERT_TRUE(pool.HasOutputFast());

	ASSERT_EQ(pool.GetOutput(), 7);
	ASSERT_FALSE(pool.HasOutput());
	ASSERT_FALSE(pool.HasOutputFast());
}

// --- clearing --------------------------------------------------------------

TEST(ThreadPoolQueues, ClearInputLeavesTheOutputQueueAlone)
{
	IntPool pool;

	pool.AddInput(AddOneHundred, 1);
	pool.AddInput(AddOneHundred, 2);
	pool.AddOutput(9);

	pool.ClearInput();

	ASSERT_EQ(pool.InputSize(), 0u);
	ASSERT_EQ(pool.OutputSize(), 1u) << "ClearInput must not touch the output queue";
	ASSERT_EQ(pool.GetOutputAtIndex(0), 9);
}

TEST(ThreadPoolQueues, ClearOutputLeavesTheInputQueueAlone)
{
	IntPool pool;

	pool.AddInput(AddOneHundred, 1);
	pool.AddOutput(9);
	pool.AddOutput(8);

	pool.ClearOutput();

	ASSERT_EQ(pool.OutputSize(), 0u);
	ASSERT_EQ(pool.InputSize(), 1u) << "ClearOutput must not touch the input queue";
	ASSERT_EQ(pool.GetInputAtIndex(0), 1);
}

// Clear() on a pool with no threads running used to return with runThreadsMutex
// still held, so the very next lock of it deadlocked -- including the one the
// destructor performs via StopThreads. This test hangs forever against that bug.
TEST(ThreadPoolQueues, ClearDoesNotLeaveRunThreadsMutexHeld)
{
	IntPool pool;

	pool.AddOutput(1);
	pool.Clear();

	// Each of these locks runThreadsMutex.
	ASSERT_FALSE(pool.WasStarted());
	pool.StopThreads();
	pool.Clear();
	ASSERT_FALSE(pool.WasStarted());

	// And so does ~ThreadPool, when pool goes out of scope.
}

TEST(ThreadPoolQueues, ClearEmptiesBothQueuesWhenNoThreadsAreRunning)
{
	IntPool pool;

	ASSERT_FALSE(pool.WasStarted());

	pool.AddInput(AddOneHundred, 1);
	pool.AddInput(AddTwoHundred, 2);
	pool.AddOutput(9);

	pool.Clear();

	ASSERT_EQ(pool.InputSize(), 0u);
	ASSERT_EQ(pool.OutputSize(), 0u);
}

TEST(ThreadPoolQueues, QueuesAreReusableAfterBeingCleared)
{
	IntPool pool;

	pool.AddInput(AddOneHundred, 1);
	pool.AddOutput(9);
	pool.Clear();

	pool.AddInput(AddOneHundred, 5);
	pool.AddOutput(6);

	ASSERT_EQ(pool.InputSize(), 1u);
	ASSERT_EQ(pool.GetInputAtIndex(0), 5);
	ASSERT_EQ(pool.OutputSize(), 1u);
	ASSERT_EQ(pool.GetOutput(), 6);
}

// --- differential stress ---------------------------------------------------

// A fixed-seed sequence of adds, indexed removals and head pops, checked against
// a std::deque reference model after every operation. Deterministic, so a
// divergence reproduces exactly; wrap-around in the old ring buffer and
// reallocation in the new deque are both exercised by the mixed load.
TEST(ThreadPoolQueues, OutputQueueMatchesADequeModelUnderMixedOperations)
{
	IntPool pool;
	std::deque<int> model;

	uint32_t seed = 0xC0FFEEu;
	int nextValue = 0;

	for (int step = 0; step < 4000; step++)
	{
		seed = seed * 1103515245u + 12345u;
		const uint32_t roll = (seed >> 16) % 100u;

		if (roll < 50u || model.empty())
		{
			const int v = nextValue++;
			pool.AddOutput(v);
			model.push_back(v);
		}
		else if (roll < 80u)
		{
			ASSERT_EQ(pool.GetOutput(), model.front()) << "step " << step;
			model.pop_front();
		}
		else
		{
			seed = seed * 1103515245u + 12345u;
			const unsigned index = (unsigned) ((seed >> 16) % (uint32_t) model.size());
			pool.RemoveOutputAtIndex(index);
			model.erase(model.begin() + index);
		}

		ASSERT_EQ(pool.OutputSize(), (unsigned) model.size()) << "step " << step;
		ASSERT_EQ(pool.HasOutput(), model.empty() == false) << "step " << step;
	}

	// Drain and compare the tail of the run element by element.
	for (size_t i = 0; i < model.size(); i++)
		ASSERT_EQ(pool.GetOutputAtIndex((unsigned) i), model[i]) << "index " << i;
}

TEST(ThreadPoolQueues, InputQueueMatchesADequeModelUnderMixedOperations)
{
	IntPool pool;
	std::deque<int> model;

	uint32_t seed = 0x5EED1234u;
	int nextValue = 0;

	for (int step = 0; step < 4000; step++)
	{
		seed = seed * 1103515245u + 12345u;
		const uint32_t roll = (seed >> 16) % 100u;

		if (roll < 60u || model.empty())
		{
			const int v = nextValue++;
			pool.AddInput((v & 1) ? AddOneHundred : AddTwoHundred, v);
			model.push_back(v);
		}
		else
		{
			seed = seed * 1103515245u + 12345u;
			const unsigned index = (unsigned) ((seed >> 16) % (uint32_t) model.size());
			pool.RemoveInputAtIndex(index);
			model.erase(model.begin() + index);
		}

		ASSERT_EQ(pool.InputSize(), (unsigned) model.size()) << "step " << step;
	}

	for (size_t i = 0; i < model.size(); i++)
		ASSERT_EQ(pool.GetInputAtIndex((unsigned) i), model[i]) << "index " << i;
}
