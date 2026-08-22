/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Regression coverage for the synchronization primitives hardened alongside
// https://github.com/MafiaHub/MafiaNet/issues/7: LocklessUint32_t (now
// std::atomic-backed), ThreadsafeAllocatingQueue's PopInaccurate (no longer
// probes the queue outside its mutex), and GetTimeUS' first-use
// initialization (now a thread-safe magic static). All tests are join-based:
// no wall-clock deadlines, deterministic assertions. Their data-race aspect
// is what TSan runs of this suite verify.

#include <gtest/gtest.h>

#include "mafianet/LocklessTypes.h"
#include "mafianet/DS_ThreadsafeAllocatingQueue.h"
#include "mafianet/GetTime.h"
#include "mafianet/thread.h"
#include "mafianet/defines.h"

#include <atomic>
#include <set>

using namespace MafiaNet;

namespace
{

const int kThreads = 8;
const int kOpsPerThread = 10000;

struct CounterJob
{
	LocklessUint32_t *counter;
	bool increment;
};

RAK_THREAD_DECLARATION(CounterThread)
{
	CounterJob *job = (CounterJob *) arguments;
	for (int i = 0; i < kOpsPerThread; i++)
	{
		if (job->increment)
			job->counter->Increment();
		else
			job->counter->Decrement();
	}
	return 0;
}

struct QueueItem
{
	unsigned value;
};

typedef DataStructures::ThreadsafeAllocatingQueue<QueueItem> ItemQueue;

struct ProducerJob
{
	ItemQueue *queue;
	unsigned firstValue; // pushes firstValue .. firstValue+kOpsPerThread-1
};

RAK_THREAD_DECLARATION(ProducerThread)
{
	ProducerJob *job = (ProducerJob *) arguments;
	for (int i = 0; i < kOpsPerThread; i++)
	{
		QueueItem *item = job->queue->Allocate(_FILE_AND_LINE_);
		item->value = job->firstValue + (unsigned) i;
		job->queue->Push(item);
	}
	return 0;
}

RAK_THREAD_DECLARATION(GetTimeThread)
{
	MafiaNet::TimeUS *out = (MafiaNet::TimeUS *) arguments;
	*out = MafiaNet::GetTimeUS();
	return 0;
}

} // namespace

TEST(LocklessUint32, ReturnsValueAfterChange)
{
	LocklessUint32_t counter;
	EXPECT_EQ(counter.GetValue(), 0u);
	// Both mutators are documented to return the value AFTER the change; the
	// old __sync_fetch_and_add implementation returned the value before it.
	EXPECT_EQ(counter.Increment(), 1u);
	EXPECT_EQ(counter.Increment(), 2u);
	EXPECT_EQ(counter.Decrement(), 1u);
	EXPECT_EQ(counter.Decrement(), 0u);
}

TEST(LocklessUint32, ConcurrentIncrementsAndDecrementsBalanceExactly)
{
	// Seed high enough that concurrent decrements can never underflow.
	LocklessUint32_t counter(kThreads * kOpsPerThread);

	CounterJob jobs[kThreads];
	RakThread::ThreadHandle handles[kThreads];
	for (int i = 0; i < kThreads; i++)
	{
		jobs[i].counter = &counter;
		jobs[i].increment = (i % 2 == 0); // half increment, half decrement
		ASSERT_EQ(RakThread::CreateJoinable(CounterThread, &jobs[i], &handles[i]), 0);
	}
	for (int i = 0; i < kThreads; i++)
		RakThread::Join(handles[i]);

	// Equal numbers of increments and decrements: no update may be lost.
	EXPECT_EQ(counter.GetValue(), (uint32_t)(kThreads * kOpsPerThread));
}

TEST(ThreadsafeAllocatingQueue, ConcurrentPushersLoseNothing)
{
	ItemQueue queue;

	ProducerJob jobs[kThreads];
	RakThread::ThreadHandle handles[kThreads];
	for (int i = 0; i < kThreads; i++)
	{
		jobs[i].queue = &queue;
		jobs[i].firstValue = (unsigned) (i * kOpsPerThread);
		ASSERT_EQ(RakThread::CreateJoinable(ProducerThread, &jobs[i], &handles[i]), 0);
	}

	// Pop concurrently with the producers through the code path the network
	// thread uses (PopInaccurate), then drain the remainder after joining.
	std::set<unsigned> seen;
	const size_t expected = (size_t) kThreads * kOpsPerThread;
	while (seen.size() < expected / 2)
	{
		QueueItem *item = queue.PopInaccurate();
		if (item == nullptr)
			continue;
		EXPECT_TRUE(seen.insert(item->value).second) << "duplicate value " << item->value;
		queue.Deallocate(item, _FILE_AND_LINE_);
	}

	for (int i = 0; i < kThreads; i++)
		RakThread::Join(handles[i]);

	// All producers done: everything not yet seen must still be in the queue.
	QueueItem *item;
	while ((item = queue.PopInaccurate()) != nullptr)
	{
		EXPECT_TRUE(seen.insert(item->value).second) << "duplicate value " << item->value;
		queue.Deallocate(item, _FILE_AND_LINE_);
	}
	EXPECT_EQ(seen.size(), expected);
}

TEST(GetTime, ConcurrentCallsShareOneTimeBase)
{
	// Exercises concurrent (potentially first) calls to GetTimeUS. Before the
	// magic-static fix a racing thread could observe a torn or stale base time
	// and return a wildly wrong timestamp.
	MafiaNet::TimeUS results[kThreads];
	RakThread::ThreadHandle handles[kThreads];
	for (int i = 0; i < kThreads; i++)
		ASSERT_EQ(RakThread::CreateJoinable(GetTimeThread, &results[i], &handles[i]), 0);
	for (int i = 0; i < kThreads; i++)
		RakThread::Join(handles[i]);

	const MafiaNet::TimeUS after = MafiaNet::GetTimeUS();
	for (int i = 0; i < kThreads; i++)
	{
		// 0 is legitimate for a caller racing the very first initialization
		// (the base time is the first call), so no lower bound beyond the type.
		// Every concurrent reading must lie in the past relative to a call made
		// after all of them completed, and within the same time base (a torn
		// base would put it minutes-to-years off).
		EXPECT_LE(results[i], after) << "thread " << i;
		EXPECT_LT(after - results[i], (MafiaNet::TimeUS) 60 * 1000000) << "thread " << i;
	}
}
