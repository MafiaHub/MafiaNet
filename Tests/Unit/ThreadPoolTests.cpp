/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/thread_pool.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

// Clear() on a pool whose threads were never started used to return with
// runThreadsMutex still held. The destructor calls Clear(), so every
// FileListTransfer destroyed a locked mutex, and any later call that takes the
// lock from another thread blocked forever.
//
// The deadline only bounds the failure: with the bug, WasStarted() never
// returns. The blocked thread is detached so a failure reports instead of
// hanging the test binary.
TEST(ThreadPool, ClearOnAnUnstartedPoolReleasesItsLock)
{
	auto pool = std::make_shared<ThreadPool<int, int>>();
	pool->Clear();

	auto done = std::make_shared<std::promise<bool>>();
	std::future<bool> wasStarted = done->get_future();
	std::thread([pool, done] { done->set_value(pool->WasStarted()); }).detach();

	ASSERT_EQ(wasStarted.wait_for(std::chrono::seconds(10)), std::future_status::ready)
		<< "WasStarted() blocked: Clear() left runThreadsMutex locked";
	EXPECT_FALSE(wasStarted.get());
}
