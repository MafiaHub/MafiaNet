/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/simple_mutex.h"
#include "mafianet/thread_pool.h"

#include <thread>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

namespace
{
// TryLock from a second thread: it never blocks, and it is not fooled by
// the re-entrant Windows lock the way a same-thread attempt would be.
bool FreeFromAnotherThread(SimpleMutex &mutex)
{
	bool acquired = false;
	std::thread([&mutex, &acquired]
	            {
			acquired = mutex.TryLock();
			if (acquired)
				mutex.Unlock(); })
	    .join();
	return acquired;
}

// ThreadPool is a header-only template, so constructing a derived pool in
// this translation unit is safe; the subclass only exposes the lock.
class InspectablePool : public ThreadPool<int, int>
{
public:
	SimpleMutex &RunThreadsMutex() { return runThreadsMutex; }
	WorkerCallback CallbackAt(unsigned index) { return inputQueue[index].first; }
};

int Doubled(int input, bool *returnOutput, void *)
{
	*returnOutput = true;
	return input * 2;
}
int Negated(int input, bool *returnOutput, void *)
{
	*returnOutput = true;
	return -input;
}
int Dropped(int, bool *returnOutput, void *)
{
	*returnOutput = false;
	return 0;
}
}

static_assert(!std::is_copy_constructible_v<ThreadPool<int, int>>, "ThreadPool owns mutexes and running threads");
static_assert(!std::is_copy_assignable_v<ThreadPool<int, int>>, "ThreadPool owns mutexes and running threads");

TEST(SimpleMutex, TryLockTakesAFreeMutex)
{
	SimpleMutex mutex;
	EXPECT_TRUE(FreeFromAnotherThread(mutex));
}

TEST(SimpleMutex, TryLockFailsWhileAnotherThreadHoldsIt)
{
	SimpleMutex mutex;
	mutex.Lock();
	EXPECT_FALSE(FreeFromAnotherThread(mutex));
	mutex.Unlock();
	EXPECT_TRUE(FreeFromAnotherThread(mutex));
}

// Clear() on a pool whose threads were never started used to return with
// runThreadsMutex still held. The destructor calls Clear(), so every
// FileListTransfer destroyed a locked mutex, and any later call that takes the
// lock from another thread blocked forever.
//
// A pool that kept its lock is leaked on purpose: its destructor calls Clear()
// again, which would deadlock re-locking the mutex and hang instead of failing.
TEST(ThreadPool, ClearOnAnUnstartedPoolReleasesItsLock)
{
	InspectablePool *pool = new InspectablePool;
	pool->Clear();
	const bool released = FreeFromAnotherThread(pool->RunThreadsMutex());
	EXPECT_TRUE(released) << "Clear() left runThreadsMutex locked";
	if (released)
		delete pool;
}

TEST(ThreadPool, InputQueueKeepsEachCallbackWithItsInput)
{
	// Never started: the queue is inspected and edited synchronously.
	InspectablePool pool;
	EXPECT_FALSE(pool.HasInputFast());
	pool.AddInput(Doubled, 1);
	pool.AddInput(Negated, 2);
	pool.AddInput(Dropped, 3);
	EXPECT_TRUE(pool.HasInputFast());
	EXPECT_TRUE(pool.HasInput());

	pool.LockInput();
	ASSERT_EQ(pool.InputSize(), 3u);
	pool.RemoveInputAtIndex(1); // removes the input and its callback together
	ASSERT_EQ(pool.InputSize(), 2u);
	EXPECT_EQ(pool.GetInputAtIndex(0), 1);
	EXPECT_EQ(pool.CallbackAt(0), &Doubled);
	EXPECT_EQ(pool.GetInputAtIndex(1), 3);
	EXPECT_EQ(pool.CallbackAt(1), &Dropped);
	pool.UnlockInput();

	pool.ClearInput();
	EXPECT_FALSE(pool.HasInputFast());
	EXPECT_FALSE(pool.HasInput());
}

TEST(ThreadPool, OutputQueueIsFifoAndGetOutputOnEmptyIsValueInitialized)
{
	ThreadPool<int, int> pool;
	EXPECT_FALSE(pool.HasOutputFast());
	pool.AddOutput(7);
	pool.AddOutput(8);
	EXPECT_TRUE(pool.HasOutputFast());
	pool.LockOutput();
	ASSERT_EQ(pool.OutputSize(), 2u);
	EXPECT_EQ(pool.GetOutputAtIndex(1), 8);
	pool.UnlockOutput();
	EXPECT_EQ(pool.GetOutput(), 7);
	EXPECT_EQ(pool.GetOutput(), 8);
	EXPECT_FALSE(pool.HasOutputFast());
	EXPECT_EQ(pool.GetOutput(), 0);
}

// Workers pop inputs and push outputs while this thread adds input and polls output, the
// HasOutputFast/HasOutput pattern the Autopatcher and Lobby2 use. Run under TSan to check the
// queues and the published counts. Terminates when every output arrived (ctest's timeout is the
// backstop), so no wall-clock assertion.
TEST(ThreadPool, WorkersProcessEveryInputExactlyOnce)
{
	ThreadPool<int, int> pool;
	ASSERT_TRUE(pool.StartThreads(3, 0));
	const int count = 2000;
	std::vector<int> seen(count, 0);
	int received = 0;
	for (int i = 0; i < count; ++i)
	{
		pool.AddInput(i % 2 ? Doubled : Negated, i);
		while (pool.HasOutputFast() && pool.HasOutput())
		{
			const int output = pool.GetOutput();
			const int input = output <= 0 ? -output : output / 2; // even inputs negated, odd doubled
			++seen[input];
			++received;
		}
	}
	while (received < count)
	{
		if (pool.HasOutputFast() && pool.HasOutput())
		{
			const int output = pool.GetOutput();
			const int input = output <= 0 ? -output : output / 2;
			++seen[input];
			++received;
		}
		else
			std::this_thread::yield();
	}
	pool.StopThreads();
	for (int i = 0; i < count; ++i)
		EXPECT_EQ(seen[i], 1) << i;
	EXPECT_FALSE(pool.HasInputFast());
	EXPECT_FALSE(pool.HasOutputFast());
}
