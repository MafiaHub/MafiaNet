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
/// A server-local number for one accepted game join. Zero is reserved.
enum class PlayerId : uint32_t
{
};

/// Own one registry per running server. The application chooses when a join
/// is accepted and when its disconnect handlers have finished. Registry
/// operations are thread-safe. Lookups return a snapshot of the mapping.
class PlayerIdRegistry
{
public:
	/// Allocate in [1, highestId], in order. After wrap, skip IDs still in use.
	explicit PlayerIdRegistry(uint32_t highestId = UINT32_MAX)
	    : maxId(highestId) {}

	/// Return the existing ID on a repeated call for an active peer.
	/// An unassigned PeerGuid or a full ID range returns std::nullopt.
	std::optional<PlayerId> Assign(PeerGuid peer)
	{
		if (peer == UNASSIGNED_PEER_GUID)
			return std::nullopt;
		std::lock_guard<std::mutex> lock(mutex);
		const auto existing = idsByPeer.find(peer);
		if (existing != idsByPeer.end())
			return existing->second;
		if (peersById.size() >= maxId)
			return std::nullopt;

		PlayerId id = next;
		while (peersById.count(id))
			id = Successor(id);

		const auto entry = peersById.emplace(id, peer).first;
		try
		{
			idsByPeer.emplace(peer, id);
		}
		catch (...)
		{
			peersById.erase(entry);
			throw;
		}
		next = Successor(id);
		return id;
	}

	std::optional<PlayerId> FindId(PeerGuid peer) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = idsByPeer.find(peer);
		if (entry == idsByPeer.end())
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

	/// Release this join after its disconnect handlers run. Unknown IDs return false.
	bool Release(PlayerId id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = peersById.find(id);
		if (entry == peersById.end())
			return false;
		idsByPeer.erase(entry->second);
		peersById.erase(entry);
		return true;
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
