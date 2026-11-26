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
/// a client's load is always the summed weight of the delegated replicas it actually holds.
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
	/// A session ended. Its grants are already gone (GrantChanged); what it held is re-elected here, at once.
	void Disconnected(PeerGuid guid);
	/// The replica is gone.
	void Forget(NetworkID id);
	void Reset();
	/// Delivers the queued changes. The caller holds no MafiaNet lock; the world guard is taken per change.
	void Deliver(std::recursive_mutex &world);

private:
	struct Entry
	{
		DelegationPolicy policy;
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
	// Bumped whenever an entry is erased, so an evaluation can tell that the veto it called broke the rules.
	uint64_t erasures = 0;
	std::deque<Notice> notices;
	bool applying = false, delivering = false;
	std::vector<DelegationCandidate> candidates;
	std::vector<std::pair<PeerGuid, SpatialPoint>> nearby;

	// The first time at or after `from` in this replica's phase of the interval.
	TimeUS NextDue(NetworkID id, TimeUS from) const;
	void Schedule(NetworkID id, Entry &entry, TimeUS due);
	void Unschedule(NetworkID id, Entry &entry);
	void Charge(Entry &entry, PeerGuid holder);
	bool Candidate(PeerGuid guid, NetworkID id) const;
	void Evaluate(NetworkID id, Entry &entry);
	// Whether the release veto is asked before a handover away from this holder.
	bool Vetoable(const Entry &entry, PeerGuid holder) const;
	void DeclineChanged(NetworkID id, const Entry &entry);
	uint32_t CapacityOf(PeerGuid guid) const;
	// Evaluates the urgent replicas; returns them.
	std::vector<NetworkID> RunUrgent();
	void MarkHeldBy(PeerGuid guid);
};
} // namespace MafiaNet
