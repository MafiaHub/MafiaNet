/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "mafianet/weighted_heap.h"

using namespace MafiaNet;

/*
Description:
Tests for the weighted min-heap the reliability layer keeps outgoing packets and per-channel
ordering in.

Written when that structure moved off DataStructures::Heap (std migration stage 1). It had no tests,
despite deciding the order packets leave the send buffer: pop the wrong element and messages go out
in the wrong order, which is not something the integration suite would reliably surface.

The load-bearing case is WeightedHeapDifferential: it drives the heap and a std::multiset model
through the same fixed-seed mix of pushes and pops and checks that every pop returns the lowest
weight still held. A heap bug that only shows up under a particular interleaving of pushes and pops
is exactly what a hand-written sequence of operations misses.
*/

namespace
{
	typedef MafiaNet::WeightedHeap<double, int> Heap;

	// Drain the heap and return weights in pop order.
	std::vector<double> DrainWeights(Heap &heap)
	{
		std::vector<double> out;
		while (heap.Size())
		{
			out.push_back(heap.PeekWeight());
			heap.Pop();
		}
		return out;
	}
} // namespace

TEST(WeightedHeap, EmptyHeapReportsNoSize)
{
	Heap heap;
	EXPECT_EQ(heap.Size(), 0u);
	EXPECT_TRUE(heap.IsEmpty());
}

TEST(WeightedHeap, SingleElementPeeksAndPops)
{
	Heap heap;
	heap.Push(4.5, 42);

	ASSERT_EQ(heap.Size(), 1u);
	EXPECT_FALSE(heap.IsEmpty());
	EXPECT_EQ(heap.Peek(), 42);
	EXPECT_DOUBLE_EQ(heap.PeekWeight(), 4.5);

	EXPECT_EQ(heap.Pop(), 42);
	EXPECT_EQ(heap.Size(), 0u);
}

TEST(WeightedHeap, PopsInAscendingWeightOrderWhateverThePushOrder)
{
	const double weights[] = {5.0, 1.0, 4.0, 2.0, 3.0};
	Heap heap;
	for (int i = 0; i < 5; ++i)
		heap.Push(weights[i], i);

	const std::vector<double> popped = DrainWeights(heap);
	ASSERT_EQ(popped.size(), (size_t)5);
	for (int i = 0; i < 5; ++i)
		EXPECT_DOUBLE_EQ(popped[i], (double)(i + 1)) << "position " << i;
}

TEST(WeightedHeap, AscendingPushesPopInTheSameOrder)
{
	Heap heap;
	for (int i = 0; i < 64; ++i)
		heap.Push((double)i, i);

	const std::vector<double> popped = DrainWeights(heap);
	ASSERT_EQ(popped.size(), (size_t)64);
	for (int i = 0; i < 64; ++i)
		EXPECT_DOUBLE_EQ(popped[i], (double)i) << "position " << i;
}

TEST(WeightedHeap, DescendingPushesStillPopAscending)
{
	Heap heap;
	for (int i = 63; i >= 0; --i)
		heap.Push((double)i, i);

	const std::vector<double> popped = DrainWeights(heap);
	ASSERT_EQ(popped.size(), (size_t)64);
	for (int i = 0; i < 64; ++i)
		EXPECT_DOUBLE_EQ(popped[i], (double)i) << "position " << i;
}

TEST(WeightedHeap, ClearDropsEverything)
{
	Heap heap;
	for (int i = 0; i < 10; ++i)
		heap.Push((double)i, i);
	ASSERT_EQ(heap.Size(), 10u);

	heap.Clear();

	EXPECT_EQ(heap.Size(), 0u);
	EXPECT_TRUE(heap.IsEmpty());
}

TEST(WeightedHeap, EqualWeightsAllComeOutAndNoneAreLost)
{
	Heap heap;
	for (int i = 0; i < 16; ++i)
		heap.Push(7.0, i);

	std::set<int> seen;
	while (heap.Size())
	{
		EXPECT_DOUBLE_EQ(heap.PeekWeight(), 7.0);
		seen.insert(heap.Pop());
	}
	EXPECT_EQ(seen.size(), (size_t)16) << "an element was lost or duplicated among equal weights";
}

// The split-packet path pushes a run of ascending weights. This was the case DataStructures::Heap
// had a dedicated StartSeries/PushSeries fast path for; measurement showed the fast path was slower
// than an ordinary push, so it is gone, but the ordering it had to preserve is asserted here.
TEST(WeightedHeap, ARunOfAscendingPushesOntoANonEmptyHeapKeepsOrder)
{
	Heap heap;
	heap.Push(100.0, 1000);
	heap.Push(200.0, 2000);

	for (int i = 0; i < 750; ++i)
		heap.Push(300.0 + (double)i, i);

	const std::vector<double> popped = DrainWeights(heap);
	ASSERT_EQ(popped.size(), (size_t)752);
	EXPECT_DOUBLE_EQ(popped[0], 100.0);
	EXPECT_DOUBLE_EQ(popped[1], 200.0);
	for (size_t i = 2; i < popped.size(); ++i)
		ASSERT_LE(popped[i - 1], popped[i]) << "pop order broke at " << i;
}

TEST(WeightedHeapDifferential, MatchesAMultisetModelUnderMixedPushAndPop)
{
	Heap heap;
	std::multiset<double> model;

	unsigned seed = 0x5eed7777u;
	for (int step = 0; step < 20000; ++step)
	{
		seed = seed * 1103515245u + 12345u;
		const unsigned roll = (seed >> 16) % 100u;

		if (roll < 60 || model.empty())
		{
			seed = seed * 1103515245u + 12345u;
			const double weight = (double)((seed >> 16) % 1000u);
			heap.Push(weight, step);
			model.insert(weight);
		}
		else
		{
			// Every pop must hand back the smallest weight the model still holds.
			const double expected = *model.begin();
			ASSERT_EQ(heap.Size(), (unsigned)model.size()) << "step " << step;
			ASSERT_DOUBLE_EQ(heap.PeekWeight(), expected) << "step " << step << ": wrong minimum";
			heap.Pop();
			model.erase(model.begin());
		}
		ASSERT_EQ(heap.Size(), (unsigned)model.size()) << "step " << step;
	}

	// Drain and confirm the tail comes out sorted.
	double prev = -1.0;
	while (heap.Size())
	{
		const double w = heap.PeekWeight();
		ASSERT_LE(prev, w) << "drain order broke";
		prev = w;
		heap.Pop();
		model.erase(model.begin());
	}
	EXPECT_TRUE(model.empty());
}
