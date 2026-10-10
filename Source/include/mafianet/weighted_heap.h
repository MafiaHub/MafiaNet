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
	/// DataStructures::List. This keeps std::push_heap/std::pop_heap on a std::vector, so the heap
	/// arithmetic is the standard library's rather than ours, while staying a small named type
	/// instead of std::priority_queue: the reliability layer needs to read the minimum's weight
	/// without popping it (PeekWeight), and measured ~1.8x slower pushes through a
	/// priority_queue of std::pair on the split-packet path.
	///
	/// The predecessor also had StartSeries()/PushSeries(), which appended without sifting while a
	/// caller promised ascending weights. It is deliberately not reproduced: benchmarking the one
	/// call site's pattern (64, 750 and 4000 fragments) showed it was never faster than an ordinary
	/// push and usually slower, while silently corrupting pop order if the promise were ever broken.
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
