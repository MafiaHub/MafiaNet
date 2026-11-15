/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief A FIFO queue guarded by one mutex, for handing items between threads.
///

#ifndef MAFIANET_LOCKED_QUEUE_H
#define MAFIANET_LOCKED_QUEUE_H

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace MafiaNet
{

/// A FIFO queue whose every operation takes one mutex, so any number of producer and
/// consumer threads may share it. There is deliberately no unlocked peek: a stale emptiness
/// check would be a data race.
///
/// Items are owned by the queue until popped. To pass heap objects between threads use
/// T = std::unique_ptr<X>: producers Push(std::make_unique<X>(...)), consumers
/// `while (auto item = queue.TryPop()) { ... }`, and whatever is still queued when the queue
/// is cleared or destroyed is freed by its unique_ptr.
template <class T>
class LockedQueue
{
public:
	LockedQueue() = default;
	LockedQueue(const LockedQueue &) = delete;
	LockedQueue &operator=(const LockedQueue &) = delete;

	/// Appends \a item at the back.
	void Push(T item)
	{
		std::lock_guard<std::mutex> lock(mutex);
		items.push_back(std::move(item));
	}

	/// Removes and returns the front item, or std::nullopt when the queue is empty.
	std::optional<T> TryPop()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (items.empty())
			return std::nullopt;
		std::optional<T> front(std::move(items.front()));
		items.pop_front();
		return front;
	}

	/// Whether the queue was empty at the moment of the call. Takes the lock.
	bool IsEmpty() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return items.empty();
	}

	/// The number of queued items at the moment of the call. Takes the lock.
	std::size_t Size() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return items.size();
	}

	/// Destroys every queued item.
	void Clear()
	{
		// Destroy the items outside the lock: an item's destructor must not be able to deadlock
		// on (or be slowed by) this queue.
		std::deque<T> discarded;
		{
			std::lock_guard<std::mutex> lock(mutex);
			discarded.swap(items);
		}
	}

private:
	mutable std::mutex mutex;
	std::deque<T> items;
};

} // namespace MafiaNet

#endif
