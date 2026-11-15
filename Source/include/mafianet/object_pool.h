/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file object_pool.h
/// \brief A free list of reusable heap objects for allocation-heavy hot paths.
///
/// Only for objects whose allocation rate showed up in the benchmarks (the
/// reliability layer's per-message bookkeeping). Elsewhere use plain new/delete
/// or std::unique_ptr.

#pragma once

#include <cstddef> // std::size_t
#include <memory>  // std::unique_ptr
#include <utility> // std::move
#include <vector>  // std::vector

namespace MafiaNet
{

/// \brief Recycles heap objects of type T instead of freeing them.
///
/// Acquire() hands out a raw pointer: the objects this pool serves live in
/// intrusive lists, heaps and fixed arrays whose owner changes as a message
/// moves through the layer, so a smart pointer would be released at almost
/// every step. The caller owns an acquired object until it passes it back to
/// Release() exactly once (or deletes it; the pool never tracks objects it has
/// handed out). The pool owns only the released objects, and frees them in
/// Clear() and its destructor.
///
/// Objects are not reconstructed on reuse: Acquire() returns either a
/// default-initialized new T or a released object as it was left. Callers must
/// set every field they read. Not thread-safe.
template <class T>
class ObjectPool
{
public:
	ObjectPool() = default;
	ObjectPool(const ObjectPool &) = delete;
	ObjectPool &operator=(const ObjectPool &) = delete;

	/// A released object if there is one, otherwise a newly allocated one.
	T *Acquire()
	{
		if (freeList.empty())
			return new T;
		T *object = freeList.back().release();
		freeList.pop_back();
		return object;
	}

	/// Takes back an object Acquire() returned, for reuse. Null is ignored.
	void Release(T *object)
	{
		std::unique_ptr<T> owned(object);
		if (owned)
			freeList.push_back(std::move(owned));
	}

	/// Frees every released object. Objects still acquired are unaffected.
	void Clear() { freeList.clear(); }

	/// The number of released objects waiting for reuse.
	std::size_t FreeCount() const { return freeList.size(); }

private:
	std::vector<std::unique_ptr<T>> freeList;
};

} // namespace MafiaNet
