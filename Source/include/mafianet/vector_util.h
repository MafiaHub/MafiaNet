/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file vector_util.h
/// \brief Small helpers for std::vector.
///
/// \ref MafiaNet::SwapRemove replaces the order-discarding "remove at index, fast" idiom of the
/// legacy DataStructures::List.

#pragma once

#include <cstddef> // std::size_t
#include <utility> // std::move
#include <vector>  // std::vector

namespace MafiaNet
{

/// \brief Remove v[index] in O(1) without keeping the order of the remaining elements.
///
/// The last element is moved into the hole, then the vector shrinks by one.
/// \pre index < v.size(). Internal use only: never call it with an index derived from network
/// data without checking it first.
template <class T, class A> void SwapRemove(std::vector<T, A> &v, std::size_t index)
{
	if (index + 1 != v.size())
		v[index] = std::move(v.back());
	v.pop_back();
}

} // namespace MafiaNet
