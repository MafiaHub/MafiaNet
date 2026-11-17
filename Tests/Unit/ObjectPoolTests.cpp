/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/object_pool.h"

#include <set>

namespace
{
// Counts live instances so a test can see what the pool frees.
struct Tracked
{
	static int live;
	int value = 0;
	Tracked() { ++live; }
	~Tracked() { --live; }
};
int Tracked::live = 0;
}

TEST(ObjectPool, AcquireReusesReleasedObjects)
{
	MafiaNet::ObjectPool<Tracked> pool;
	Tracked *first = pool.Acquire();
	first->value = 42;
	pool.Release(first);
	EXPECT_EQ(pool.FreeCount(), 1u);

	Tracked *again = pool.Acquire();
	EXPECT_EQ(again, first);
	EXPECT_EQ(again->value, 42); // not reconstructed: callers set what they read
	EXPECT_EQ(pool.FreeCount(), 0u);
	pool.Release(again);
}

TEST(ObjectPool, AcquireAfterExhaustionAllocates)
{
	const int before = Tracked::live;
	MafiaNet::ObjectPool<Tracked> pool;
	Tracked *a = pool.Acquire();
	pool.Release(a);
	Tracked *reused = pool.Acquire();
	Tracked *fresh = pool.Acquire(); // free list is empty again
	EXPECT_EQ(reused, a);
	EXPECT_NE(fresh, reused);
	EXPECT_EQ(Tracked::live, before + 2);
	pool.Release(reused);
	pool.Release(fresh);
	EXPECT_EQ(pool.FreeCount(), 2u);
}

TEST(ObjectPool, DestroyingThePoolFreesAll)
{
	const int before = Tracked::live;
	{
		MafiaNet::ObjectPool<Tracked> pool;
		std::set<Tracked *> distinct;
		Tracked *objects[8];
		for (Tracked *&object : objects)
		{
			object = pool.Acquire();
			distinct.insert(object);
		}
		EXPECT_EQ(distinct.size(), 8u);
		for (Tracked *object : objects)
			pool.Release(object);
		EXPECT_EQ(Tracked::live, before + 8);
	}
	EXPECT_EQ(Tracked::live, before);
}

TEST(ObjectPool, ClearFreesReleasedButNotAcquiredObjects)
{
	const int before = Tracked::live;
	MafiaNet::ObjectPool<Tracked> pool;
	Tracked *kept = pool.Acquire();
	Tracked *spareA = pool.Acquire();
	Tracked *spareB = pool.Acquire();
	pool.Release(spareA);
	pool.Release(spareB);
	EXPECT_EQ(Tracked::live, before + 3);
	pool.Clear();
	EXPECT_EQ(pool.FreeCount(), 0u);
	EXPECT_EQ(Tracked::live, before + 1); // the acquired one is still the caller's
	pool.Release(kept);
}

TEST(ObjectPool, ReleaseOfNullIsIgnored)
{
	MafiaNet::ObjectPool<Tracked> pool;
	pool.Release(nullptr);
	EXPECT_EQ(pool.FreeCount(), 0u);
}
