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
				mutex.Unlock();
		}).join();
		return acquired;
	}

	// ThreadPool is a header-only template, so constructing a derived pool in
	// this translation unit is safe; the subclass only exposes the lock.
	class InspectablePool : public ThreadPool<int, int>
	{
	public:
		SimpleMutex &RunThreadsMutex() { return runThreadsMutex; }
	};
}

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
