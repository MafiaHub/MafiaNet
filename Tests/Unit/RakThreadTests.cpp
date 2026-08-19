/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/thread.h"

#include <atomic>

using namespace MafiaNet;

namespace
{

RAK_THREAD_DECLARATION(IncrementCounterThread)
{
	std::atomic<int> *counter = (std::atomic<int> *) arguments;
	counter->fetch_add(1);
	return 0;
}

RAK_THREAD_DECLARATION(IncrementCounterManyTimesThread)
{
	std::atomic<int> *counter = (std::atomic<int> *) arguments;
	for (int i = 0; i < 1000; i++)
		counter->fetch_add(1);
	return 0;
}

} // namespace

TEST(RakThread, JoinWaitsForThreadCompletion)
{
	std::atomic<int> counter(0);

	RakThread::ThreadHandle handle;
	int errorCode = RakThread::CreateJoinable(IncrementCounterManyTimesThread, &counter, &handle);
	ASSERT_EQ(errorCode, 0) << "CreateJoinable failed";

	RakThread::Join(handle);

	// Join must not return before the thread function has fully completed, and
	// it must establish a happens-before edge making the thread's writes visible.
	EXPECT_EQ(counter.load(), 1000);
}

TEST(RakThread, JoinReapsMultipleThreadsIndependently)
{
	std::atomic<int> counterA(0);
	std::atomic<int> counterB(0);

	RakThread::ThreadHandle handleA;
	RakThread::ThreadHandle handleB;
	ASSERT_EQ(RakThread::CreateJoinable(IncrementCounterThread, &counterA, &handleA), 0);
	ASSERT_EQ(RakThread::CreateJoinable(IncrementCounterThread, &counterB, &handleB), 0);

	RakThread::Join(handleB);
	EXPECT_EQ(counterB.load(), 1);

	RakThread::Join(handleA);
	EXPECT_EQ(counterA.load(), 1);
}
