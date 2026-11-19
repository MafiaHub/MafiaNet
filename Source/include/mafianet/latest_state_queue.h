// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#pragma once

#include "mafianet/peer_interface.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace MafiaNet
{
/// Application-owned pending snapshots for one recipient. Each record contains
/// its own object identity, generation and complete value. The receiver can
/// apply any record independently and ignore older generations after reordering.
class RAK_DLL_EXPORT LatestStateQueue
{
public:
	/// Conservative size of one unfragmented message at the peer's current MTU,
	/// allowing for an encrypted session. Includes the application's message ID.
	static unsigned PayloadBudget(int mtu);

	/// Admit a record or replace this object's unsent value, retaining FIFO
	/// order across distinct objects. Failure preserves the previous record.
	bool Put(uint64_t key, const unsigned char *record, size_t size, unsigned payloadBudget);

	/// Forget an unsent record, for example when the object leaves visibility.
	void Erase(uint64_t key);
	size_t Size() const { return pending.size(); }

	enum class Result
	{
		Empty,
		Queued,
		Busy,
		Disconnected,
		RecordTooLarge
	};

	/// Call once per network pump. Sends at most one unreliable packet and waits
	/// for previously enqueued High priority traffic to drain. On RecordTooLarge,
	/// the MTU has shrunk: repack or erase the front object (returned in key).
	Result SendOne(RakPeerInterface &peer, SystemAddress target, MessageID messageId, uint64_t &key);

	/// Iterate over the bytes after the message ID. Returns false for an invalid
	/// frame (or at the end). A valid record is a view into the input packet.
	static bool ReadRecord(const unsigned char *&data, size_t &remaining, const unsigned char *&record, size_t &size);

private:
	std::deque<uint64_t> order;
	std::unordered_map<uint64_t, std::vector<unsigned char>> pending;
};
}
