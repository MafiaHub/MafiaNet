/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <memory>
#include <optional>

#include "mafianet/locked_queue.h"

using namespace MafiaNet;

namespace
{
// Counts its destructions so a test can see which items the queue freed.
struct Tracked
{
	explicit Tracked(int *counter)
	    : destroyed(counter) {}
	~Tracked() { ++*destroyed; }
	Tracked(const Tracked &) = delete;
	Tracked &operator=(const Tracked &) = delete;
	int *destroyed;
};
}

TEST(LockedQueue, PopsInFifoOrder)
{
	LockedQueue<int> queue;
	queue.Push(1);
	queue.Push(2);
	queue.Push(3);
	EXPECT_EQ(queue.Size(), 3u);
	EXPECT_EQ(queue.TryPop(), std::optional<int>(1));
	EXPECT_EQ(queue.TryPop(), std::optional<int>(2));
	EXPECT_EQ(queue.TryPop(), std::optional<int>(3));
	EXPECT_EQ(queue.TryPop(), std::nullopt);
	EXPECT_TRUE(queue.IsEmpty());
}

TEST(LockedQueue, TryPopOnEmptyReturnsNullopt)
{
	LockedQueue<int> queue;
	EXPECT_TRUE(queue.IsEmpty());
	EXPECT_EQ(queue.Size(), 0u);
	EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(LockedQueue, HoldsMoveOnlyItems)
{
	LockedQueue<std::unique_ptr<int>> queue;
	queue.Push(std::make_unique<int>(7));
	queue.Push(std::make_unique<int>(8));
	auto first = queue.TryPop();
	ASSERT_TRUE(first.has_value());
	ASSERT_NE(*first, nullptr);
	EXPECT_EQ(**first, 7);
	auto second = queue.TryPop();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(**second, 8);
	EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(LockedQueue, DestroyingANonEmptyQueueFreesItems)
{
	int destroyed = 0;
	{
		LockedQueue<std::unique_ptr<Tracked>> queue;
		for (int i = 0; i < 5; i++)
			queue.Push(std::make_unique<Tracked>(&destroyed));
		EXPECT_EQ(destroyed, 0);
	}
	EXPECT_EQ(destroyed, 5);
}

TEST(LockedQueue, ClearFreesItemsAndEmptiesTheQueue)
{
	int destroyed = 0;
	LockedQueue<std::unique_ptr<Tracked>> queue;
	for (int i = 0; i < 3; i++)
		queue.Push(std::make_unique<Tracked>(&destroyed));
	queue.Clear();
	EXPECT_EQ(destroyed, 3);
	EXPECT_TRUE(queue.IsEmpty());
	EXPECT_FALSE(queue.TryPop().has_value());
}
