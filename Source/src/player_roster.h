// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/types.h"
#include "mafianet/session_admission.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <list>
#include <memory>
#include <string>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MafiaNet
{
namespace detail
{
enum class SessionDecision
{
	Accept,
	Reject,
	Status
};
struct PendingSession
{
	PeerGuid guid;
	uint64_t generation;
	SystemAddress address;
	std::string payload;
};
}
using PendingSessionPtr = std::shared_ptr<const detail::PendingSession>;

// Network-thread connection transitions and application-thread admissions share this lock.
class PlayerRoster
{
public:
	/// Allocate in [1, highestId], in order. After wrap, skip IDs still in use.
	explicit PlayerRoster(uint32_t highestId = UINT32_MAX)
	    : maxId(highestId) {}

	void SetIncoming(unsigned int connected, unsigned int reserved)
	{
		std::lock_guard<std::mutex> lock(mutex);
		limits.connected = connected;
		limits.reserved = std::min(reserved, connected);
	}

	void SetPending(unsigned short total, unsigned short perAddress)
	{
		std::lock_guard<std::mutex> lock(mutex);
		limits.pending = total;
		limits.pending_per_address = perAddress;
	}

	ServerCapacity Capacity() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return limits;
	}

	void Pending(PeerGuid peer, uint64_t generation, const SystemAddress &address)
	{
		if (peer == UNASSIGNED_PEER_GUID)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		auto &entry = connections.try_emplace(peer).first->second;
		Release(entry);
		entry = Entry{};
		entry.generation = generation;
		entry.phase = Phase::Pending;
		entry.address = address;
	}

	// Publish in callback order. A request can already have timed out while its packet waited for poll().
	void Publish(const PendingSessionPtr &request)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = connections.find(request->guid);
		if (it != connections.end() && it->second.generation == request->generation && it->second.phase == Phase::Pending && !it->second.waiting)
			it->second.waiting = waiting.insert(waiting.end(), request);
	}

	std::vector<PendingSessionPtr> PendingRequests() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return {waiting.begin(), waiting.end()};
	}

	// Pin commands submitted by address or GUID to the same generation as the capacity claim.
	template <class Enqueue>
	SessionAcceptResult Answer(const AddressOrGUID &target, uint64_t generation, detail::SessionDecision decision, ConnectionSlot slot, Enqueue enqueue)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = connections.find(ToPeerGuid(target.rakNetGuid));
		if (target.rakNetGuid == UNASSIGNED_RAKNET_GUID && target.systemAddress != UNASSIGNED_SYSTEM_ADDRESS)
			it = std::find_if(connections.begin(), connections.end(), [&](const auto &pair)
			                  { return pair.second.address == target.systemAddress && pair.second.phase == Phase::Pending; });
		if (it == connections.end() || (generation && it->second.generation != generation) || it->second.phase != Phase::Pending)
			return SessionAcceptResult::Expired;
		auto &entry = it->second;
		const bool accepting = decision == detail::SessionDecision::Accept;
		if (accepting && (claimed >= limits.connected || (slot == ConnectionSlot::Public && publicClaimed >= limits.connected - limits.reserved)))
			return SessionAcceptResult::Full;
		// Commit the decision only once the queue owns the command. The consumer takes this
		// same roster lock to publish a connected peer or release its claim.
		enqueue(entry.generation);
		if (accepting)
		{
			entry.slot = slot;
			++claimed;
			if (slot == ConnectionSlot::Public)
				++publicClaimed;
		}
		if (decision != detail::SessionDecision::Status)
		{
			RemoveWaiting(entry);
			entry.phase = Phase::Answered;
		}
		return SessionAcceptResult::Accepted;
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
		Release(entry->second);
		connections.erase(entry);
		return id;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		claimed = 0;
		publicClaimed = 0;
		connections.clear();
		waiting.clear();
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
		SystemAddress address;
		std::optional<ConnectionSlot> slot;
		std::optional<std::list<PendingSessionPtr>::iterator> waiting;
	};
	void RemoveWaiting(Entry &entry)
	{
		if (entry.waiting)
		{
			waiting.erase(*entry.waiting);
			entry.waiting.reset();
		}
	}

	void Release(Entry &entry)
	{
		RemoveWaiting(entry);
		peersById.erase(entry.id);
		if (entry.slot)
		{
			--claimed;
			if (*entry.slot == ConnectionSlot::Public)
				--publicClaimed;
		}
	}
	std::list<PendingSessionPtr> waiting;
	ServerCapacity limits{0};
	unsigned int claimed = 0;
	unsigned int publicClaimed = 0;
	std::unordered_map<PeerGuid, Entry> connections;
	std::map<PlayerId, PeerGuid> peersById;
};
}
