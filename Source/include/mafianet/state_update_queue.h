// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#pragma once

#include "mafianet/peer_interface.h"

#include <list>
#include <unordered_map>
#include <vector>

namespace MafiaNet
{
/// Application-owned pending snapshots for one recipient. Each record contains
/// its own object identity, generation and complete value. The receiver can
/// apply any record independently and ignore older generations after reordering.
class RAK_DLL_EXPORT StateUpdateQueue
{
public:
	enum class Delivery
	{
		ReliableOrdered,
		LatestOnly
	};

	/// ReliableOrdered keeps every admitted record in FIFO order. LatestOnly
	/// replaces pending values per object and sends them unreliably. Once full,
	/// Put rejects new entries without eviction so the producer can retry.
	explicit StateUpdateQueue(size_t maxPendingRecords, Delivery delivery = Delivery::ReliableOrdered, char orderingChannel = 0)
	    : maxPendingRecords(maxPendingRecords), delivery(delivery), orderingChannel(orderingChannel) {}
	StateUpdateQueue(const StateUpdateQueue &) = delete;
	StateUpdateQueue &operator=(const StateUpdateQueue &) = delete;

	/// Conservative size of one unfragmented message at the peer's current MTU,
	/// allowing for an encrypted session. Includes the application's message ID.
	static unsigned PayloadBudget(int mtu);

	/// Returns false for a full queue or invalid size. The caller retains any
	/// record that could not be admitted.
	bool Put(uint64_t key, const unsigned char *record, size_t size, unsigned payloadBudget);

	/// In LatestOnly mode, forget an object that leaves visibility.
	bool Erase(uint64_t key);
	size_t Size() const { return pending.size(); }

	enum class Result
	{
		Empty,
		Queued,
		Busy,
		Disconnected,
		RecordTooLarge
	};

	/// Call once per network pump. Sends at most one packed message and waits
	/// for previously enqueued High priority traffic to drain. On RecordTooLarge,
	/// the record no longer fits. LatestOnly can replace it. ReliableOrdered
	/// sends an already admitted record on its own using reliable fragmentation.
	Result SendOne(RakPeerInterface &peer, SystemAddress target, MessageID messageId, uint64_t &key);

	/// Iterate over the bytes after the message ID. Returns false for an invalid
	/// frame (or at the end). A valid record is a view into the input packet.
	static bool ReadRecord(const unsigned char *&data, size_t &remaining, const unsigned char *&record, size_t &size);

private:
	struct Record
	{
		uint64_t key;
		std::vector<unsigned char> bytes;
	};
	size_t maxPendingRecords;
	Delivery delivery;
	char orderingChannel;
	std::list<Record> pending;
	std::unordered_map<uint64_t, std::list<Record>::iterator> latest;
};
}
