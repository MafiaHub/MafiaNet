// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/delegation.h"
#include "mafianet/time.h"
#include "mafianet/types.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MafiaNet
{
class ManagedReplication;
class Replica3;

/// Server side of delegation: which client simulates each opted-in replica. Owned by ManagedReplication and used
/// under its world guard. It hands replicas over only through ManagedReplication::Assign, so the epoch, revocation
/// and eligibility rules of SetAuthority apply unchanged, and it follows every grant change through GrantChanged, so
/// a client's load is always the summed weight of the managed replicas it actually holds.
///
/// Managed replicas are the delegated ones and the members and leaders of delegation groups. A group is the tree of
/// followers under a root (its leader that follows nobody); it has one holder. A delegated root is elected once for
/// the group, with its policy and position and the group's summed weight, by candidates eligible for every member; a
/// root that is not delegated is the game's, and its followers take whatever holder it has. Pins, declines, refusals
/// and the schedule live on the root; a member's own policy only gives its weight.
class DelegationService final
{
public:
	using Callback = std::function<void(NetworkID, PeerGuid, PeerGuid, DelegationChange)>;
	using Veto = std::function<bool(NetworkID, PeerGuid)>;
	/// Decline changes ask for a pass at most this often (V1 kDeclinedPassGapMs): the message is not rate limited.
	static constexpr TimeUS kDeclinedPassGapUs = 100000;

	explicit DelegationService(ManagedReplication &manager)
	    : manager(manager) {}

	bool Set(Replica3 &replica, const DelegationPolicy *policy);
	void SetInterval(uint32_t milliseconds);
	void SetDefaultCapacity(uint32_t maxWeight) { defaultCapacity = maxWeight; }
	bool SetCapacity(PeerGuid guid, uint32_t maxWeight);
	bool Suspend(PeerGuid guid);
	bool Resume(PeerGuid guid);
	bool IsSuspended(PeerGuid guid) const { return suspended.count(guid) != 0; }
	bool Pin(Replica3 &replica, PeerGuid guid);
	bool Unpin(Replica3 &replica);
	bool IsPinned(Replica3 &replica) const;
	/// Links \a member under \a leader (null: unlinks it), and sets the matching interest grid dependency.
	bool SetGroup(Replica3 &member, Replica3 *leader);
	void SetCallback(Callback next) { callback = next ? std::make_shared<Callback>(std::move(next)) : nullptr; }
	void SetReleaseVeto(Veto next) { veto = next ? std::make_shared<Veto>(std::move(next)) : nullptr; }
	/// A client declined a replica it has constructed, or withdrew that. Ignored for a replica that is not delegated.
	void Declined(NetworkID id, PeerGuid guid, bool declined);
	/// The replica left the client's visible set: its decline is forgotten.
	void LeftScope(NetworkID id, PeerGuid guid);
	DelegationStatistics Statistics() const;
	uint32_t Load(PeerGuid guid) const;

	/// From Tick, after the interest rebuild: the replicas due this tick and every urgent one.
	void Pass();
	/// Every grant change on a delegated replica, whoever made it. Not the delegation's own: it rechecks at once.
	void GrantChanged(NetworkID id, PeerGuid current);
	/// A session ended. Its grants are already gone (GrantChanged); the groups of the replicas it \a held, and those
	/// pinned to it, are re-elected here, at once. A group a live client still holds that would need its veto, and
	/// everything else, waits for Tick: the veto is never asked from here.
	void Disconnected(PeerGuid guid, const std::vector<NetworkID> &held);
	/// The replica is gone.
	void Forget(NetworkID id);
	void Reset();
	/// Delivers the queued changes. The caller holds no MafiaNet lock; the world guard is taken per change.
	void Deliver(std::recursive_mutex &world);

private:
	struct Entry
	{
		DelegationPolicy policy;
		// Opted in with SetDelegation; otherwise managed only as a group member or leader.
		bool delegated = false;
		// The replica it follows, and those that follow it.
		NetworkID leader = UNASSIGNED_NETWORK_ID;
		std::vector<NetworkID> followers;
		// Who carries this replica's weight: its actual grant holder.
		PeerGuid charged = UNASSIGNED_PEER_GUID;
		// The holder the callback last announced, so a change made outside a pass is reported relative to it.
		PeerGuid reported = UNASSIGNED_PEER_GUID;
		TimeUS due = 0;
		bool scheduled = false;
		bool pinned = false;
		PeerGuid pinnedTo = UNASSIGNED_PEER_GUID;
		// The holder's veto has refused the handover the election wants since refusingSince.
		bool refusing = false;
		TimeUS refusingSince = 0;
		// Clients that declined it; each is passed over by the election until it withdraws, loses sight of the
		// replica or disconnects. A pin ignores them.
		std::vector<PeerGuid> declined;
	};
	struct Notice
	{
		NetworkID id;
		PeerGuid previous, current;
		DelegationChange change;
	};
	ManagedReplication &manager;
	std::unordered_map<NetworkID, Entry> entries;
	// The staggered schedule: (due time, replica). Each replica's due time keeps the phase its NetworkID hashes to.
	std::set<std::pair<TimeUS, NetworkID>> schedule;
	// Re-elected at the next pass whatever the schedule says.
	std::set<NetworkID> urgent;
	std::unordered_map<PeerGuid, uint32_t> loads;
	std::unordered_map<PeerGuid, uint32_t> capacities;
	std::unordered_set<PeerGuid> suspended;
	uint32_t defaultCapacity = 0;
	TimeUS intervalUs = 500000;
	DelegationStatistics stats;
	std::shared_ptr<Callback> callback;
	std::shared_ptr<Veto> veto;
	// Replicas whose declines changed since the last decline pass, and when that pass ran.
	std::set<NetworkID> declineChanged;
	TimeUS lastDeclinePass = 0;
	bool declinePassRan = false;
	// Bumped whenever an entry, a group link, a pin or a grant changes, so an evaluation can tell that the veto it
	// called broke the rules.
	uint64_t revision = 0;
	std::deque<Notice> notices;
	bool applying = false, delivering = false, disconnecting = false;
	std::vector<DelegationCandidate> candidates;
	// The group being evaluated, root first, and what its members declined.
	std::vector<NetworkID> group;
	std::vector<PeerGuid> groupDeclined;
	struct Move
	{
		Replica3 *replica;
		NetworkID id;
		PeerGuid previous;
	};
	std::vector<Move> targets, moved;
	std::vector<std::pair<PeerGuid, SpatialPoint>> nearby;

	// The first time at or after `from` in this replica's phase of the interval.
	TimeUS NextDue(NetworkID id, TimeUS from) const;
	void Schedule(NetworkID id, Entry &entry, TimeUS due);
	void Unschedule(NetworkID id, Entry &entry);
	static uint32_t Weight(const Entry &entry) { return entry.delegated ? entry.policy.weight : DelegationPolicy{}.weight; }
	void Charge(Entry &entry, PeerGuid holder);
	bool Candidate(PeerGuid guid, NetworkID id) const;
	// Candidate for every replica of `group`.
	bool GroupCandidate(PeerGuid guid) const;
	// The entry for a managed replica, created on first use with its current holder.
	Entry &Track(NetworkID id);
	NetworkID RootOf(NetworkID id) const;
	// Has its own pass: a root that is delegated or leads a group.
	static bool IsHead(const Entry &entry) { return entry.leader == UNASSIGNED_NETWORK_ID && (entry.delegated || !entry.followers.empty()); }
	// Schedules, unschedules or erases the entry after a change to its policy or links.
	void Refresh(NetworkID id);
	void Erase(NetworkID id);
	void Unlink(NetworkID member);
	void Evaluate(NetworkID id, Entry &entry);
	// Whether the release veto is asked before a handover of `group` away from this holder.
	bool Vetoable(PeerGuid holder) const;
	void DeclineChanged(NetworkID id);
	uint32_t CapacityOf(PeerGuid guid) const;
	// Evaluates the urgent replicas; returns them.
	std::vector<NetworkID> RunUrgent();
	void MarkHeldBy(PeerGuid guid);
};
} // namespace MafiaNet
