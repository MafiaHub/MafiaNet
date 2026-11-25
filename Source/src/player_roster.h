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

	void Pending(PeerGuid peer, uint64_t generation)
	{
		if (peer == UNASSIGNED_PEER_GUID)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		connections.insert_or_assign(peer, Entry{generation, Phase::Pending});
	}

	bool Answer(PeerGuid peer, uint64_t generation, bool final)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = connections.find(peer);
		if (it == connections.end() || it->second.generation != generation || it->second.phase != Phase::Pending)
			return false;
		if (final)
			it->second.phase = Phase::Answered;
		return true;
	}

	void Connected(PeerGuid peer, uint64_t generation = 0, int ping = -1)
	{
		if (peer == UNASSIGNED_PEER_GUID)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		auto &entry = connections.try_emplace(peer).first->second;
		entry.generation = generation;
		entry.phase = Phase::Connected;
		entry.ping = ping;
	}

	std::optional<PlayerId> Accept(PeerGuid peer, uint64_t generation = 0)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = connections.find(peer);
		if (entry == connections.end() || entry->second.phase != Phase::Connected || (generation && entry->second.generation != generation))
			return std::nullopt;
		if (entry->second.id != PlayerId{})
			return entry->second.id;
		if (peersById.size() >= maxId)
			return std::nullopt;

		PlayerId id = next;
		while (peersById.count(id))
			id = Successor(id);

		peersById.emplace(id, peer);
		entry->second.id = id;
		next = Successor(id);
		return id;
	}

	std::optional<PlayerId> FindId(PeerGuid peer) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = connections.find(peer);
		if (entry == connections.end() || entry->second.id == PlayerId{})
			return std::nullopt;
		return entry->second.id;
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

	// Only internal snapshot construction runs under the lock.
	template <class Append>
	void VisitPlayers(Append append) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto &[id, guid] : peersById)
			append(id, guid, connections.at(guid).generation);
	}

	struct ConnectionInfo
	{
		PlayerId id;
		int ping;
	};

	std::optional<ConnectionInfo> FindConnection(PeerGuid guid, uint64_t generation) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = connections.find(guid);
		if (it == connections.end() || it->second.generation != generation || it->second.phase != Phase::Connected)
			return std::nullopt;
		return ConnectionInfo{it->second.id, it->second.ping};
	}

	void UpdatePing(PeerGuid guid, uint64_t generation, int ping)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = connections.find(guid);
		if (it != connections.end() && it->second.generation == generation)
			it->second.ping = ping;
	}

	PlayerId Disconnected(PeerGuid peer, uint64_t generation = 0)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = connections.find(peer);
		if (entry == connections.end() || (generation && entry->second.generation != generation))
			return {};
		const PlayerId id = entry->second.id;
		peersById.erase(id);
		connections.erase(entry);
		return id;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		connections.clear();
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
	enum class Phase
	{
		Pending,
		Answered,
		Connected
	};
	struct Entry
	{
		uint64_t generation = 0;
		Phase phase = Phase::Connected;
		PlayerId id{};
		int ping = -1;
	};
	std::unordered_map<PeerGuid, Entry> connections;
	std::map<PlayerId, PeerGuid> peersById;
};
}
