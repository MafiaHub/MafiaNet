// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/types.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MafiaNet
{
// Network-thread connection transitions and application-thread admissions share this lock.
class PlayerRoster
{
public:
	/// Allocate in [1, highestId], in order. After wrap, skip IDs still in use.
	explicit PlayerRoster(uint32_t highestId = UINT32_MAX)
	    : maxId(highestId) {}

	void Connected(PeerGuid peer)
	{
		if (peer == UNASSIGNED_PEER_GUID)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		idsByPeer.try_emplace(peer, PlayerId{});
	}

	std::optional<PlayerId> Accept(PeerGuid peer)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = idsByPeer.find(peer);
		if (entry == idsByPeer.end())
			return std::nullopt;
		if (entry->second != PlayerId{})
			return entry->second;
		if (peersById.size() >= maxId)
			return std::nullopt;

		PlayerId id = next;
		while (peersById.count(id))
			id = Successor(id);

		peersById.emplace(id, peer);
		entry->second = id;
		next = Successor(id);
		return id;
	}

	std::optional<PlayerId> FindId(PeerGuid peer) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = idsByPeer.find(peer);
		if (entry == idsByPeer.end() || entry->second == PlayerId{})
			return std::nullopt;
		return entry->second;
	}

	std::optional<PeerGuid> FindPeer(PlayerId id) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = peersById.find(id);
		if (entry == peersById.end())
			return std::nullopt;
		return entry->second;
	}

	/// Replace out with a snapshot of joined players, in ascending player ID order.
	void GetPlayerList(std::vector<std::pair<PlayerId, PeerGuid>> &out) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		out.assign(peersById.begin(), peersById.end());
	}

	PlayerId Disconnected(PeerGuid peer)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = idsByPeer.find(peer);
		if (entry == idsByPeer.end())
			return {};
		const PlayerId id = entry->second;
		peersById.erase(id);
		idsByPeer.erase(entry);
		return id;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		idsByPeer.clear();
		peersById.clear();
		next = PlayerId{1};
	}

private:
	PlayerId Successor(PlayerId id) const
	{
		const auto value = static_cast<uint32_t>(id);
		return PlayerId{value == maxId ? 1u : value + 1u};
	}

	const uint32_t maxId;
	PlayerId next{1};
	mutable std::mutex mutex;
	std::unordered_map<PeerGuid, PlayerId> idsByPeer;
	std::map<PlayerId, PeerGuid> peersById;
};
}
