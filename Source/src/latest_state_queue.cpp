// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#include "mafianet/latest_state_queue.h"
#include "mafianet/statistics.h"
#include <algorithm>

namespace MafiaNet
{
bool LatestStateQueue::Put(uint64_t key, const unsigned char *record, size_t size, unsigned payloadBudget)
{
	if (!record || !size || size > UINT16_MAX || size + 3 > payloadBudget)
		return false;
	auto entry = pending.try_emplace(key);
	if (entry.second)
		order.push_back(key);
	entry.first->second.assign(record, record + size);
	return true;
}

void LatestStateQueue::Erase(uint64_t key)
{
	if (pending.erase(key))
		order.erase(std::find(order.begin(), order.end(), key));
}

LatestStateQueue::Result LatestStateQueue::SendOne(RakPeerInterface &peer, SystemAddress target, MessageID messageId, uint64_t &key)
{
	if (order.empty())
		return Result::Empty;
	RakNetStatistics stats;
	if (target == UNASSIGNED_SYSTEM_ADDRESS || !peer.GetStatistics(target, &stats))
		return Result::Disconnected;
	if (stats.messageInSendBuffer[static_cast<unsigned>(Priority::High)] != 0)
		return Result::Busy;
	const auto budget = PayloadBudget(peer.GetMTUSize(target));
	std::vector<unsigned char> packet{messageId};
	size_t count = 0;
	for (const auto object : order)
	{
		const auto &record = pending.at(object);
		if (packet.size() + 2 + record.size() > budget)
		{
			if (count == 0)
			{
				key = object;
				return Result::RecordTooLarge;
			}
			break;
		}
		packet.push_back(static_cast<unsigned char>(record.size() >> 8));
		packet.push_back(static_cast<unsigned char>(record.size()));
		packet.insert(packet.end(), record.begin(), record.end());
		++count;
	}
	if (!peer.Send(reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), Priority::High, Reliability::Unreliable, 0, target, false))
		return Result::Disconnected;
	while (count--)
	{
		pending.erase(order.front());
		order.pop_front();
	}
	return Result::Queued;
}

bool LatestStateQueue::ReadRecord(const unsigned char *&data, size_t &remaining, const unsigned char *&record, size_t &size)
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
