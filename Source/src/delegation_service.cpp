// Copyright (c) 2026, MafiaHub. MIT license.
#include "delegation_service.h"
#include "managed_replication.h"
#include "steady_time.h"
#include <algorithm>

namespace MafiaNet
{
namespace
{
// splitmix64's finaliser: NetworkIDs are sequential, and their phases must still spread over the interval.
uint64_t Mix(uint64_t value)
{
	value += 0x9e3779b97f4a7c15ull;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
	return value ^ (value >> 31);
}
} // namespace

TimeUS DelegationService::NextDue(NetworkID id, TimeUS from) const
{
	if (intervalUs == 0)
	{
		return from;
	}
	const TimeUS phase = Mix(id) % intervalUs;
	const TimeUS past = (from % intervalUs + intervalUs - phase) % intervalUs;
	return past == 0 ? from : from - past + intervalUs;
}

void DelegationService::Schedule(NetworkID id, Entry &entry, TimeUS due)
{
	Unschedule(id, entry);
	entry.due = due;
	entry.scheduled = true;
	schedule.emplace(due, id);
}

void DelegationService::Unschedule(NetworkID id, Entry &entry)
{
	if (entry.scheduled)
	{
		schedule.erase({entry.due, id});
		entry.scheduled = false;
	}
}

void DelegationService::Charge(Entry &entry, PeerGuid holder)
{
	if (entry.charged != UNASSIGNED_PEER_GUID)
	{
		auto load = loads.find(entry.charged);
		if (load != loads.end())
		{
			load->second -= (std::min)(load->second, entry.policy.weight);
			if (load->second == 0)
			{
				loads.erase(load);
			}
		}
	}
	entry.charged = holder;
	if (holder != UNASSIGNED_PEER_GUID)
	{
		loads[holder] += entry.policy.weight;
	}
}

uint32_t DelegationService::CapacityOf(PeerGuid guid) const
{
	auto it = capacities.find(guid);
	return it == capacities.end() ? defaultCapacity : it->second;
}

uint32_t DelegationService::Load(PeerGuid guid) const
{
	auto it = loads.find(guid);
	return it == loads.end() ? 0 : it->second;
}

bool DelegationService::Candidate(PeerGuid guid, NetworkID id) const
{
	// Exactly what Assign checks for a client, so an elected winner is grantable, plus the delegation's own filter.
	return guid != UNASSIGNED_PEER_GUID && !suspended.count(guid) && manager.Status(guid) == ReplicationPeerStatus::Active && manager.interest.Eligible(guid, id);
}

bool DelegationService::Set(Replica3 &replica, const DelegationPolicy *policy)
{
	if (!manager.Delegable(replica))
	{
		return false;
	}
	const NetworkID id = replica.GetNetworkID();
	auto it = entries.find(id);
	if (!policy)
	{
		// Opting out leaves the holder in place; from now on the game manages it by hand.
		if (it != entries.end())
		{
			Charge(it->second, UNASSIGNED_PEER_GUID);
			Unschedule(id, it->second);
			urgent.erase(id);
			declineChanged.erase(id);
			entries.erase(it);
			++erasures;
		}
		return true;
	}
	if (it == entries.end())
	{
		it = entries.emplace(id, Entry{}).first;
		it->second.policy = policy->Normalized();
		const PeerGuid holder = manager.HolderOf(id);
		Charge(it->second, holder);
		it->second.reported = holder;
		Schedule(id, it->second, NextDue(id, GetSteadyTimeUS()));
		return true;
	}
	const PeerGuid holder = it->second.charged;
	Charge(it->second, UNASSIGNED_PEER_GUID);
	it->second.policy = policy->Normalized();
	Charge(it->second, holder);
	return true;
}

void DelegationService::SetInterval(uint32_t milliseconds)
{
	intervalUs = static_cast<TimeUS>(milliseconds) * 1000;
	const TimeUS now = GetSteadyTimeUS();
	for (auto &[id, entry] : entries)
	{
		Schedule(id, entry, NextDue(id, now));
	}
}

bool DelegationService::SetCapacity(PeerGuid guid, uint32_t maxWeight)
{
	if (!manager.sessions.count(guid))
	{
		return false;
	}
	capacities[guid] = maxWeight;
	return true;
}

void DelegationService::MarkHeldBy(PeerGuid guid)
{
	for (const auto &[id, entry] : entries)
	{
		if (entry.charged == guid || (entry.pinned && entry.pinnedTo == guid))
		{
			urgent.insert(id);
		}
	}
}

bool DelegationService::Suspend(PeerGuid guid)
{
	if (!manager.sessions.count(guid))
	{
		return false;
	}
	if (suspended.insert(guid).second)
	{
		MarkHeldBy(guid);
	}
	return true;
}

bool DelegationService::Resume(PeerGuid guid)
{
	if (!suspended.erase(guid))
	{
		return false;
	}
	// A pin to it waited for it; ranges wait for the schedule.
	for (const auto &[id, entry] : entries)
	{
		if (entry.pinned && entry.pinnedTo == guid)
		{
			urgent.insert(id);
		}
	}
	return true;
}

bool DelegationService::Pin(Replica3 &replica, PeerGuid guid)
{
	if (!manager.Delegable(replica) || (guid != UNASSIGNED_PEER_GUID && !manager.sessions.count(guid)))
	{
		return false;
	}
	const NetworkID id = replica.GetNetworkID();
	auto it = entries.find(id);
	if (it == entries.end())
	{
		return false;
	}
	it->second.pinned = true;
	it->second.pinnedTo = guid;
	urgent.insert(id);
	return true;
}

bool DelegationService::Unpin(Replica3 &replica)
{
	auto it = entries.find(replica.GetNetworkID());
	if (!manager.Delegable(replica) || it == entries.end() || !it->second.pinned)
	{
		return false;
	}
	it->second.pinned = false;
	it->second.pinnedTo = UNASSIGNED_PEER_GUID;
	urgent.insert(it->first);
	return true;
}

bool DelegationService::IsPinned(Replica3 &replica) const
{
	auto it = entries.find(replica.GetNetworkID());
	return it != entries.end() && it->second.pinned;
}

DelegationStatistics DelegationService::Statistics() const
{
	DelegationStatistics out = stats;
	out.simulated = 0;
	for (const auto &entry : entries)
	{
		if (entry.second.charged != UNASSIGNED_PEER_GUID)
		{
			++out.simulated;
		}
	}
	out.dormant = entries.size() - out.simulated;
	return out;
}

bool DelegationService::Vetoable(const Entry &entry, PeerGuid holder) const
{
	// Nobody to refuse for, a suspension (never vetoed), or a holder that gave the replica up itself.
	return veto && holder != UNASSIGNED_PEER_GUID && !suspended.count(holder) && std::find(entry.declined.begin(), entry.declined.end(), holder) == entry.declined.end();
}

void DelegationService::Declined(NetworkID id, PeerGuid guid, bool declined)
{
	auto it = entries.find(id);
	if (it == entries.end())
	{
		return;
	}
	auto &list = it->second.declined;
	const auto at = std::find(list.begin(), list.end(), guid);
	if ((at != list.end()) == declined)
	{
		return;
	}
	if (declined)
	{
		list.push_back(guid);
		++stats.declines;
	}
	else
	{
		list.erase(at);
	}
	DeclineChanged(id, it->second);
}

void DelegationService::LeftScope(NetworkID id, PeerGuid guid)
{
	auto it = entries.find(id);
	if (it == entries.end())
	{
		return;
	}
	auto &list = it->second.declined;
	const auto at = std::find(list.begin(), list.end(), guid);
	if (at != list.end())
	{
		list.erase(at);
		DeclineChanged(id, it->second);
	}
}

void DelegationService::DeclineChanged(NetworkID id, const Entry &entry)
{
	// Coalesced into a pass at most every kDeclinedPassGapUs. A pinned replica's holder does not depend on it.
	if (!entry.pinned)
	{
		declineChanged.insert(id);
	}
}

void DelegationService::Evaluate(NetworkID id, Entry &entry)
{
	++stats.elections;
	Replica3 *replica = manager.owner.FindReplica(id);
	if (!replica)
	{
		return;
	}
	const PeerGuid current = entry.charged;
	PeerGuid desired = UNASSIGNED_PEER_GUID;
	SpatialScope scope;
	if (entry.pinned)
	{
		// A pin overrides the ranges, not existence: a pin naming a client that cannot hold it now leaves it dormant.
		desired = Candidate(entry.pinnedTo, id) ? entry.pinnedTo : UNASSIGNED_PEER_GUID;
	}
	else if (manager.interest.ScopeOf(id, scope))
	{
		// Only the clients within the release range can win: nobody farther is kept or acquires.
		manager.interest.RadiusObserversNear(scope.position, entry.policy.releaseRange, nearby);
		candidates.clear();
		for (const auto &[guid, position] : nearby)
		{
			if (Candidate(guid, id))
			{
				DelegationCandidate candidate;
				candidate.guid = guid;
				candidate.position = position;
				candidate.load = Load(guid);
				candidate.capacity = CapacityOf(guid);
				candidates.push_back(candidate);
			}
		}
		desired = ElectSimulator(entry.policy, scope.position, current, candidates, entry.declined);
	}
	bool forcing = false, postponed = false;
	if (desired != current && Vetoable(entry, current))
	{
		const uint64_t before = erasures;
		const std::shared_ptr<Veto> ask = veto;
		const bool allowed = (*ask)(id, current);
		// The veto must not change replication state. If it did anyway, this evaluation is void: the replica is
		// elected again at the next pass, from whatever state the veto left.
		if (erasures != before || manager.owner.FindReplica(id) != replica || entry.charged != current)
		{
			if (entries.count(id))
			{
				urgent.insert(id);
			}
			return;
		}
		if (allowed)
		{
			entry.refusing = false;
		}
		else
		{
			const TimeUS now = GetSteadyTimeUS();
			if (!entry.refusing)
			{
				entry.refusing = true;
				entry.refusingSince = now;
			}
			// The grace stops a replica that is always busy from staying with a holder the election has left behind.
			forcing = now - entry.refusingSince >= static_cast<TimeUS>(entry.policy.graceMs) * 1000;
			if (!forcing)
			{
				++stats.refusals;
				postponed = true;
			}
		}
	}
	if (desired == current)
	{
		entry.refusing = false;
	}
	else if (!postponed)
	{
		// GrantChanged charges the new holder and ends the refusal. A refused grant leaves everything as it was, for
		// the next pass.
		applying = true;
		const bool assigned = manager.Assign(*replica, desired);
		applying = false;
		if (assigned)
		{
			++stats.handovers;
			if (forcing)
			{
				++stats.forced;
			}
		}
	}
	if (entry.charged != entry.reported)
	{
		DelegationChange change = DelegationChange::Migrated;
		if (entry.charged == UNASSIGNED_PEER_GUID)
		{
			change = DelegationChange::Dormant;
		}
		else if (entry.reported == UNASSIGNED_PEER_GUID)
		{
			change = DelegationChange::Acquired;
		}
		notices.push_back({id, entry.reported, entry.charged, change});
		entry.reported = entry.charged;
	}
}

std::vector<NetworkID> DelegationService::RunUrgent()
{
	std::vector<NetworkID> ran(urgent.begin(), urgent.end());
	urgent.clear();
	for (NetworkID id : ran)
	{
		auto it = entries.find(id);
		if (it != entries.end())
		{
			Evaluate(id, it->second);
		}
	}
	return ran;
}

void DelegationService::Pass()
{
	// A pass asked for from inside a callback waits for the next Tick.
	if (delivering || entries.empty())
	{
		return;
	}
	const TimeUS now = GetSteadyTimeUS();
	if (!declineChanged.empty() && (!declinePassRan || now - lastDeclinePass >= kDeclinedPassGapUs))
	{
		urgent.insert(declineChanged.begin(), declineChanged.end());
		declineChanged.clear();
		declinePassRan = true;
		lastDeclinePass = now;
	}
	const std::vector<NetworkID> ran = RunUrgent();
	while (!schedule.empty() && schedule.begin()->first <= now)
	{
		const NetworkID id = schedule.begin()->second;
		Entry &entry = entries.at(id);
		Schedule(id, entry, NextDue(id, now + 1));
		if (!std::binary_search(ran.begin(), ran.end(), id))
		{
			Evaluate(id, entry);
		}
	}
}

void DelegationService::GrantChanged(NetworkID id, PeerGuid current)
{
	auto it = entries.find(id);
	if (it == entries.end() || it->second.charged == current)
	{
		return;
	}
	Charge(it->second, current);
	// A refusal is the holder's; a new holder starts with none.
	it->second.refusing = false;
	if (!applying)
	{
		// Revoked by MafiaNet (the holder became ineligible) or changed by the game: elect again at the next pass.
		urgent.insert(id);
	}
}

void DelegationService::Disconnected(PeerGuid guid)
{
	suspended.erase(guid);
	capacities.erase(guid);
	for (auto &[id, entry] : entries)
	{
		entry.declined.erase(std::remove(entry.declined.begin(), entry.declined.end(), guid), entry.declined.end());
		// A pin is to a client, and that client has left.
		if (entry.pinned && entry.pinnedTo == guid)
		{
			entry.pinned = false;
			entry.pinnedTo = UNASSIGNED_PEER_GUID;
			urgent.insert(id);
		}
	}
	// Until re-elected, what it held has an owner that can never report again.
	if (!delivering)
	{
		RunUrgent();
	}
}

void DelegationService::Forget(NetworkID id)
{
	auto it = entries.find(id);
	if (it == entries.end())
	{
		return;
	}
	Charge(it->second, UNASSIGNED_PEER_GUID);
	Unschedule(id, it->second);
	urgent.erase(id);
	declineChanged.erase(id);
	entries.erase(it);
	++erasures;
}

void DelegationService::Reset()
{
	entries.clear();
	++erasures;
	schedule.clear();
	urgent.clear();
	declineChanged.clear();
	declinePassRan = false;
	loads.clear();
	capacities.clear();
	suspended.clear();
	notices.clear();
}

void DelegationService::Deliver(std::recursive_mutex &world)
{
	{
		std::lock_guard<std::recursive_mutex> lock(world);
		if (delivering || notices.empty())
		{
			return;
		}
		delivering = true;
	}
	struct DeliveryScope
	{
		DelegationService &service;
		std::recursive_mutex &world;
		~DeliveryScope()
		{
			std::lock_guard<std::recursive_mutex> lock(world);
			service.delivering = false;
		}
	} scope{*this, world};
	for (;;)
	{
		Notice notice{};
		std::shared_ptr<Callback> target;
		{
			std::lock_guard<std::recursive_mutex> lock(world);
			if (notices.empty())
			{
				return;
			}
			notice = notices.front();
			notices.pop_front();
			target = callback;
		}
		if (target)
		{
			(*target)(notice.id, notice.previous, notice.current, notice.change);
		}
	}
}
} // namespace MafiaNet
