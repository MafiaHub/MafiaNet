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
			load->second -= (std::min)(load->second, Weight(entry));
			if (load->second == 0)
			{
				loads.erase(load);
			}
		}
	}
	entry.charged = holder;
	if (holder != UNASSIGNED_PEER_GUID)
	{
		loads[holder] += Weight(entry);
	}
}

DelegationService::Entry &DelegationService::Track(NetworkID id)
{
	auto it = entries.find(id);
	if (it == entries.end())
	{
		it = entries.emplace(id, Entry{}).first;
		const PeerGuid holder = manager.HolderOf(id);
		Charge(it->second, holder);
		it->second.reported = holder;
	}
	return it->second;
}

NetworkID DelegationService::RootOf(NetworkID id) const
{
	for (auto it = entries.find(id); it != entries.end() && it->second.leader != UNASSIGNED_NETWORK_ID; it = entries.find(id))
	{
		id = it->second.leader;
	}
	return id;
}

void DelegationService::Refresh(NetworkID id)
{
	auto it = entries.find(id);
	if (it == entries.end())
	{
		return;
	}
	Entry &entry = it->second;
	if (!entry.delegated && entry.leader == UNASSIGNED_NETWORK_ID && entry.followers.empty())
	{
		// Nothing manages it any more: its holder stays, from now on the game's.
		Erase(id);
	}
	else if (IsHead(entry))
	{
		if (!entry.scheduled)
		{
			Schedule(id, entry, NextDue(id, GetSteadyTimeUS()));
		}
	}
	else
	{
		// A member: its group's root carries the schedule, the pin and the pending passes.
		Unschedule(id, entry);
		urgent.erase(id);
		declineChanged.erase(id);
		entry.pinned = false;
		entry.pinnedTo = UNASSIGNED_PEER_GUID;
		entry.refusing = false;
	}
}

void DelegationService::Erase(NetworkID id)
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
	++revision;
}

void DelegationService::Unlink(NetworkID member)
{
	Entry &entry = entries.at(member);
	const NetworkID leader = entry.leader;
	entry.leader = UNASSIGNED_NETWORK_ID;
	auto &followers = entries.at(leader).followers;
	followers.erase(std::remove(followers.begin(), followers.end(), member), followers.end());
	++revision;
	Refresh(leader);
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

bool DelegationService::GroupCandidate(PeerGuid guid) const
{
	for (NetworkID id : group)
	{
		if (!Candidate(guid, id))
		{
			return false;
		}
	}
	return true;
}

bool DelegationService::Set(Replica3 &replica, const DelegationPolicy *policy)
{
	if (!manager.Delegable(replica))
	{
		return false;
	}
	const NetworkID id = replica.GetNetworkID();
	auto it = entries.find(id);
	if (!policy && (it == entries.end() || !it->second.delegated))
	{
		return true;
	}
	++revision;
	Entry &entry = it == entries.end() ? Track(id) : it->second;
	// Recharged with the new weight; the holder stays.
	const PeerGuid holder = entry.charged;
	Charge(entry, UNASSIGNED_PEER_GUID);
	if (policy)
	{
		entry.policy = policy->Normalized();
		if (!entry.delegated)
		{
			entry.delegated = true;
			entry.reported = holder;
		}
	}
	else
	{
		// Opting out leaves the holder in place; from now on the game manages it by hand (and a group it leads
		// follows that).
		entry.delegated = false;
		entry.pinned = false;
		entry.pinnedTo = UNASSIGNED_PEER_GUID;
		entry.refusing = false;
	}
	Charge(entry, holder);
	Refresh(id);
	return true;
}

bool DelegationService::SetGroup(Replica3 &member, Replica3 *leader)
{
	if (!manager.Delegable(member) || (leader && !manager.Delegable(*leader)))
	{
		return false;
	}
	const NetworkID id = member.GetNetworkID();
	if (!leader)
	{
		auto it = entries.find(id);
		if (it == entries.end() || it->second.leader == UNASSIGNED_NETWORK_ID)
		{
			return true;
		}
		const NetworkID root = RootOf(id);
		const NetworkID previous = it->second.leader;
		Unlink(id);
		// Only the dependency this link set: one the game set itself since then stays.
		if (manager.interest.GetDependency(id) == previous)
		{
			manager.interest.SetDependency(id, UNASSIGNED_NETWORK_ID);
		}
		// Its own policy, or the followers it leads, take over at the next pass; the old group is checked too.
		Refresh(id);
		if (entries.count(id))
		{
			urgent.insert(id);
		}
		if (entries.count(root))
		{
			urgent.insert(root);
		}
		return true;
	}
	const NetworkID to = leader->GetNetworkID();
	// A cycle: the member already leads the leader, directly or not.
	for (NetworkID up = to; up != UNASSIGNED_NETWORK_ID;)
	{
		if (up == id)
		{
			return false;
		}
		auto it = entries.find(up);
		up = it == entries.end() ? UNASSIGNED_NETWORK_ID : it->second.leader;
	}
	auto it = entries.find(id);
	if (it != entries.end() && it->second.leader == to)
	{
		return true;
	}
	if (!manager.interest.SetDependency(id, to))
	{
		return false;
	}
	++revision;
	Entry &entry = Track(id);
	Track(to).followers.push_back(id);
	if (entry.leader != UNASSIGNED_NETWORK_ID)
	{
		Unlink(id);
	}
	entry.leader = to;
	Refresh(id);
	Refresh(to);
	urgent.insert(RootOf(to));
	return true;
}

void DelegationService::SetInterval(uint32_t milliseconds)
{
	intervalUs = static_cast<TimeUS>(milliseconds) * 1000;
	const TimeUS now = GetSteadyTimeUS();
	for (auto &[id, entry] : entries)
	{
		if (IsHead(entry))
		{
			Schedule(id, entry, NextDue(id, now));
		}
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
			urgent.insert(RootOf(id));
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
		++revision;
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
	if (!manager.Delegable(replica) || (guid != UNASSIGNED_PEER_GUID && !manager.sessions.count(guid)) || !entries.count(replica.GetNetworkID()))
	{
		return false;
	}
	// A member pins its group, which only an elected root can be.
	const NetworkID root = RootOf(replica.GetNetworkID());
	Entry &entry = entries.at(root);
	if (!entry.delegated)
	{
		return false;
	}
	++revision;
	entry.pinned = true;
	entry.pinnedTo = guid;
	urgent.insert(root);
	return true;
}

bool DelegationService::Unpin(Replica3 &replica)
{
	if (!manager.Delegable(replica) || !entries.count(replica.GetNetworkID()))
	{
		return false;
	}
	const NetworkID root = RootOf(replica.GetNetworkID());
	Entry &entry = entries.at(root);
	if (!entry.pinned)
	{
		return false;
	}
	++revision;
	entry.pinned = false;
	entry.pinnedTo = UNASSIGNED_PEER_GUID;
	urgent.insert(root);
	return true;
}

bool DelegationService::IsPinned(Replica3 &replica) const
{
	if (!entries.count(replica.GetNetworkID()))
	{
		return false;
	}
	return entries.at(RootOf(replica.GetNetworkID())).pinned;
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

bool DelegationService::Vetoable(PeerGuid holder) const
{
	// Nobody to refuse for, a suspension (never vetoed), or a holder that gave up a replica of the group itself.
	if (!veto || holder == UNASSIGNED_PEER_GUID || suspended.count(holder))
	{
		return false;
	}
	for (NetworkID id : group)
	{
		const auto &declined = entries.at(id).declined;
		if (std::find(declined.begin(), declined.end(), holder) != declined.end())
		{
			return false;
		}
	}
	return true;
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
	DeclineChanged(id);
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
		DeclineChanged(id);
	}
}

void DelegationService::DeclineChanged(NetworkID id)
{
	// Coalesced into a pass at most every kDeclinedPassGapUs, for the whole group. A pinned group's holder does not
	// depend on it.
	const NetworkID root = RootOf(id);
	if (!entries.at(root).pinned)
	{
		declineChanged.insert(root);
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
	group.assign(1, id);
	for (size_t i = 0; i < group.size(); ++i)
	{
		// One by one: GCC 13 reports a false -Wstringop-overflow for a range insert here.
		for (NetworkID follower : entries.at(group[i]).followers)
		{
			group.push_back(follower);
		}
	}
	// Only a delegated root is the election's. The followers of any other root take its holder, whoever set it.
	const bool elected = entry.delegated;
	const PeerGuid current = entry.charged;
	PeerGuid desired = UNASSIGNED_PEER_GUID;
	SpatialScope scope;
	if (entry.pinned)
	{
		// A pin overrides the ranges, not existence: a pin naming a client that cannot hold it now leaves it dormant.
		desired = GroupCandidate(entry.pinnedTo) ? entry.pinnedTo : UNASSIGNED_PEER_GUID;
	}
	else if (!elected)
	{
		desired = GroupCandidate(current) ? current : UNASSIGNED_PEER_GUID;
	}
	else if (manager.interest.ScopeOf(id, scope))
	{
		DelegationPolicy policy = entry.policy;
		const std::vector<PeerGuid> *declined = &entry.declined;
		if (group.size() > 1)
		{
			// One election for the group: the root's policy and position, the group's weight and declines.
			policy.weight = 0;
			groupDeclined.clear();
			for (NetworkID member : group)
			{
				const Entry &each = entries.at(member);
				policy.weight += Weight(each);
				groupDeclined.insert(groupDeclined.end(), each.declined.begin(), each.declined.end());
			}
			declined = &groupDeclined;
		}
		// Only the clients within the release range can win: nobody farther is kept or acquires.
		manager.interest.RadiusObserversNear(scope.position, policy.releaseRange, nearby);
		candidates.clear();
		for (const auto &[guid, position] : nearby)
		{
			if (GroupCandidate(guid))
			{
				DelegationCandidate candidate;
				candidate.guid = guid;
				candidate.position = position;
				candidate.load = Load(guid);
				candidate.capacity = CapacityOf(guid);
				candidates.push_back(candidate);
			}
		}
		desired = ElectSimulator(policy, scope.position, current, candidates, *declined);
	}
	bool forcing = false, postponed = false;
	if (elected && desired != current && Vetoable(current))
	{
		if (disconnecting)
		{
			// A live client still holds the group (a member was the departed client's): its veto is asked at the next
			// Tick, never from a disconnect.
			urgent.insert(id);
			return;
		}
		// Asked once for the group, with the root's id: the group changes hands as one.
		const uint64_t before = revision;
		const std::shared_ptr<Veto> ask = veto;
		const bool allowed = (*ask)(id, current);
		// The veto must not change replication state. If it did anyway, this evaluation is void: the group is
		// elected again at the next pass, from whatever state the veto left.
		if (revision != before || manager.owner.FindReplica(id) != replica)
		{
			if (entries.count(id))
			{
				urgent.insert(RootOf(id));
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
	// The replicas the delegation moves: the whole group, or only the followers of a root the game owns.
	targets.clear();
	for (size_t i = elected ? 0 : 1; i < group.size(); ++i)
	{
		Replica3 *each = i == 0 ? replica : manager.owner.FindReplica(group[i]);
		if (each)
		{
			targets.push_back({each, group[i], entries.at(group[i]).charged});
		}
	}
	if (!postponed)
	{
		// All or nothing. GrantChanged charges each new holder and ends its refusal. If a grant is refused, the ones
		// this pass made are undone and the group stays as it was, for the next pass.
		const bool refusing = entry.refusing;
		const TimeUS refusingSince = entry.refusingSince;
		moved.clear();
		bool complete = true;
		applying = true;
		for (const Move &target : targets)
		{
			if (target.previous == desired)
			{
				continue;
			}
			if (!manager.Assign(*target.replica, desired))
			{
				complete = false;
				break;
			}
			moved.push_back(target);
		}
		if (!complete)
		{
			for (auto undo = moved.rbegin(); undo != moved.rend(); ++undo)
			{
				// Back to the previous holder, or to the server if that client cannot have it any more: never split.
				if (!manager.Assign(*undo->replica, undo->previous))
				{
					manager.Assign(*undo->replica, UNASSIGNED_PEER_GUID);
					// Split between the server and a client: mended at the next Tick, not the next interval.
					urgent.insert(id);
				}
			}
			entry.refusing = refusing;
			entry.refusingSince = refusingSince;
		}
		applying = false;
		if (complete)
		{
			stats.handovers += moved.size();
			if (forcing)
			{
				stats.forced += moved.size();
			}
		}
	}
	if (!elected)
	{
		// The game changed the root's holder, not the delegation: nothing to report for it.
		entry.reported = entry.charged;
	}
	for (const Move &target : targets)
	{
		Entry &each = entries.at(target.id);
		if (each.charged == each.reported)
		{
			continue;
		}
		DelegationChange change = DelegationChange::Migrated;
		if (each.charged == UNASSIGNED_PEER_GUID)
		{
			change = DelegationChange::Dormant;
		}
		else if (each.reported == UNASSIGNED_PEER_GUID)
		{
			change = DelegationChange::Acquired;
		}
		notices.push_back({target.id, each.reported, each.charged, change});
		each.reported = each.charged;
	}
}

std::vector<NetworkID> DelegationService::RunUrgent()
{
	std::vector<NetworkID> ran(urgent.begin(), urgent.end());
	urgent.clear();
	for (NetworkID id : ran)
	{
		auto it = entries.find(id);
		if (it != entries.end() && IsHead(it->second))
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
	++revision;
	// A refusal is the holder's; a new holder starts with none.
	it->second.refusing = false;
	if (!applying)
	{
		// Revoked by MafiaNet (the holder became ineligible) or changed by the game: the group is elected again (or
		// follows its root's new holder) at the next pass.
		urgent.insert(RootOf(id));
	}
}

void DelegationService::Disconnected(PeerGuid guid, const std::vector<NetworkID> &held)
{
	suspended.erase(guid);
	capacities.erase(guid);
	std::set<NetworkID> heads;
	for (NetworkID id : held)
	{
		if (entries.count(id))
		{
			heads.insert(RootOf(id));
		}
	}
	for (auto &[id, entry] : entries)
	{
		entry.declined.erase(std::remove(entry.declined.begin(), entry.declined.end(), guid), entry.declined.end());
		// A pin is to a client, and that client has left.
		if (entry.pinned && entry.pinnedTo == guid)
		{
			entry.pinned = false;
			entry.pinnedTo = UNASSIGNED_PEER_GUID;
			heads.insert(id);
		}
	}
	if (delivering)
	{
		urgent.insert(heads.begin(), heads.end());
		return;
	}
	// Until re-elected, what it held has an owner that can never report again. Only that: the other pending passes
	// (and the veto they may ask) wait for Tick.
	disconnecting = true;
	for (NetworkID id : heads)
	{
		urgent.erase(id);
		auto it = entries.find(id);
		if (it != entries.end() && IsHead(it->second))
		{
			Evaluate(id, it->second);
		}
	}
	disconnecting = false;
}

void DelegationService::Forget(NetworkID id)
{
	auto it = entries.find(id);
	if (it == entries.end())
	{
		return;
	}
	if (it->second.leader != UNASSIGNED_NETWORK_ID)
	{
		const NetworkID root = RootOf(id);
		Unlink(id);
		if (entries.count(root))
		{
			urgent.insert(root);
		}
	}
	// Its followers lead themselves now (the grid drops their dependency with it), each keeping its holder.
	const std::vector<NetworkID> followers = std::move(entries.at(id).followers);
	entries.at(id).followers.clear();
	for (NetworkID follower : followers)
	{
		entries.at(follower).leader = UNASSIGNED_NETWORK_ID;
		Refresh(follower);
		if (entries.count(follower))
		{
			urgent.insert(follower);
		}
	}
	Erase(id);
}

void DelegationService::Reset()
{
	entries.clear();
	++revision;
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
