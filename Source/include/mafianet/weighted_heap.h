/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file weighted_heap.h
/// \brief A minimum-first binary heap over std::vector, used for outgoing packet ordering.

#ifndef __WEIGHTED_HEAP_H
#define __WEIGHTED_HEAP_H

#include <algorithm>
#include <utility>
#include <vector>

#include "mafianet/assert.h"

namespace MafiaNet
{
	/// \brief Pops the element with the lowest weight first.
	///
	/// Replaces DataStructures::Heap, which was a hand-written sift-up/sift-down over
	/// DataStructures::List. std::push_heap/std::pop_heap over a std::vector put the heap
	/// arithmetic in the standard library rather than here.
	///
	/// It stays a small named type instead of std::priority_queue for a functional reason, not a
	/// performance one: the reliability layer reads the minimum's weight without popping it
	/// (PeekWeight) and sweeps every queued element by index to free it at teardown
	/// (operator[]), and std::priority_queue's interface exposes neither -- reaching its
	/// container means deriving from it to get at the protected member. A priority_queue of
	/// this same node measured indistinguishable from this class at every queue depth tried.
	///
	/// The predecessor also had StartSeries()/PushSeries(), which appended without sifting while
	/// a caller promised ascending weights. It is not reproduced because it is unsound, not
	/// because it was slow: it silently corrupts pop order if the promise is ever broken. In a
	/// microbenchmark of the one call site's pattern it was in fact up to ~35% faster than an
	/// ordinary push, but end to end through two ReliabilityLayer instances its advantage did not
	/// show above measurement noise, so the safety is free.
	///
	/// Measured, comparing this against DataStructures::Heap end to end on the send path (two
	/// ReliabilityLayer instances over a fake socket with simulated time, Release, best of 25):
	/// deep backlogs and fragment bursts up to ~4000 fragments came out at 0.97-1.00x, and the
	/// lightest case (two small messages per tick) at 1.07x. The last figure is a per-operation
	/// overhead of vector-backed storage at tiny queue depths that no implementation tried could
	/// remove -- std::push_heap, three hole-method variants, an explicit-count variant and
	/// std::priority_queue all measured the same -- and it is a fraction of a percent of a real
	/// send, which this harness omits the syscall for.
	template <class weight_type, class data_type>
	class WeightedHeap
	{
	public:
		WeightedHeap() {}
		~WeightedHeap() {}

		void Push(const weight_type &weight, const data_type &data)
		{
			heap.push_back(Node(weight, data));
			std::push_heap(heap.begin(), heap.end(), Greater());
		}

		/// \brief Lowest-weight element, which must exist.
		const data_type &Peek(void) const
		{
			RakAssert(heap.empty()==false);
			return heap.front().data;
		}

		/// \brief Weight of the lowest-weight element, which must exist.
		const weight_type &PeekWeight(void) const
		{
			RakAssert(heap.empty()==false);
			return heap.front().weight;
		}

		/// \brief Removes and returns the lowest-weight element, which must exist.
		data_type Pop(void)
		{
			RakAssert(heap.empty()==false);
			std::pop_heap(heap.begin(), heap.end(), Greater());
			data_type out = heap.back().data;
			heap.pop_back();
			return out;
		}

		void Clear(void)
		{
			heap.clear();
		}

		unsigned Size(void) const
		{
			return (unsigned) heap.size();
		}

		bool IsEmpty(void) const
		{
			return heap.empty();
		}

		/// \brief Element at a position in the backing array, in heap order rather than weight order.
		/// For sweeps over everything held -- freeing on teardown, scanning for stale entries -- where
		/// the caller does not care about ordering. Same contract as the structure this replaced.
		const data_type &operator[](unsigned index) const
		{
			RakAssert(index < heap.size());
			return heap[index].data;
		}

		data_type &operator[](unsigned index)
		{
			RakAssert(index < heap.size());
			return heap[index].data;
		}

	private:
		struct Node
		{
			Node() {}
			Node(const weight_type &w, const data_type &d) : weight(w), data(d) {}
			weight_type weight;
			data_type data;
		};

		// std::push_heap builds a maximum-first heap, so inverting the comparison gives
		// minimum-first, matching the isMaxHeap=false instantiations this replaces.
		struct Greater
		{
			bool operator()(const Node &a, const Node &b) const { return b.weight < a.weight; }
		};

		std::vector<Node> heap;
	};
}

#endif
