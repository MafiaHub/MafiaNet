// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#include "mafianet/state_update_queue.h"
#include "mafianet/statistics.h"
#include <iterator>

namespace MafiaNet
{
bool StateUpdateQueue::Put(uint64_t key, const unsigned char *record, size_t size, unsigned payloadBudget)
{
	if (!record || !size || size > UINT16_MAX || size + 3 > payloadBudget)
		return false;
	if (delivery == Delivery::LatestOnly)
	{
		const auto entry = latest.find(key);
		if (entry != latest.end())
		{
			entry->second->bytes.assign(record, record + size);
			return true;
		}
	}
	if (pending.size() >= maxPendingRecords)
		return false;
	pending.push_back({key, {record, record + size}});
	if (delivery == Delivery::LatestOnly)
		latest.emplace(key, std::prev(pending.end()));
	return true;
}

bool StateUpdateQueue::Erase(uint64_t key)
{
	const auto entry = latest.find(key);
	if (entry == latest.end())
		return false;
	pending.erase(entry->second);
	latest.erase(entry);
	return true;
}

StateUpdateQueue::Result StateUpdateQueue::SendOne(RakPeerInterface &peer, SystemAddress target, MessageID messageId, uint64_t &key)
{
	if (pending.empty())
		return Result::Empty;
	RakNetStatistics stats;
	if (target == UNASSIGNED_SYSTEM_ADDRESS || peer.GetConnectionState(target) != IS_CONNECTED || !peer.GetStatistics(target, &stats))
		return Result::Disconnected;
	const int mtu = peer.GetMTUSize(target);
	const auto high = static_cast<unsigned>(Priority::High);
	// Keep reliable records flowing behind a partial datagram. Latest-only
	// records stay replaceable until the preceding packet leaves the queue.
	const bool busy = delivery == Delivery::ReliableOrdered ? stats.bytesInSendBuffer[high] >= mtu : stats.messageInSendBuffer[high] != 0;
	if (busy)
		return Result::Busy;
	const auto budget = PayloadBudget(mtu);
	std::vector<unsigned char> packet{messageId};
	size_t count = 0;
	for (const auto &entry : pending)
	{
		const auto &record = entry.bytes;
		if (packet.size() + 2 + record.size() > budget)
		{
			if (count != 0)
				break;
			if (delivery == Delivery::LatestOnly)
			{
				key = entry.key;
				return Result::RecordTooLarge;
			}
		}
		packet.push_back(static_cast<unsigned char>(record.size() >> 8));
		packet.push_back(static_cast<unsigned char>(record.size()));
		packet.insert(packet.end(), record.begin(), record.end());
		++count;
	}
	const auto reliability = delivery == Delivery::LatestOnly ? Reliability::Unreliable : Reliability::ReliableOrdered;
	if (!peer.Send(reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), Priority::High, reliability, orderingChannel, target, false))
		return Result::Disconnected;
	for (size_t sent = 0; sent < count; ++sent)
	{
		if (delivery == Delivery::LatestOnly)
			latest.erase(pending.front().key);
		pending.pop_front();
	}
	return Result::Queued;
}

bool StateUpdateQueue::ReadRecord(const unsigned char *&data, size_t &remaining, const unsigned char *&record, size_t &size)
{
	if (!data || remaining < 2)
		return false;
	const size_t length = (static_cast<size_t>(data[0]) << 8) | data[1];
	if (!length || length > remaining - 2)
		return false;
	record = data + 2;
	size = length;
	data += 2 + length;
	remaining -= 2 + length;
	return true;
}
}
