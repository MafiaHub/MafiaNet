// Copyright (c) 2026, MafiaHub. MIT license.

#pragma once

#include "mafianet/export.h"
#include "mafianet/bit_stream.h"
#include "mafianet/interest_grid.h"
#include "mafianet/delegation.h"

#include "mafianet/types.h"

#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <typeindex>
#include <vector>
#include "mafianet/time.h"
#include <utility>

namespace MafiaNet
{
class RakPeer;
class RakPeerInterface;
class Replica3;
class NetworkIDManager;
class ManagedReplication;
class RM3SerializeExecutor;
struct SnapshotReplicaTag
{
	static constexpr std::size_t SnapshotHistoryCapacity = 0;
	virtual void ResetSnapshotHistory() {}

protected:
	~SnapshotReplicaTag() = default;
};
enum class ChangeDetection
{
	Compare,
	Explicit
};
enum class SnapshotDelivery
{
	Reliable,
	Replaceable
};
enum class ReplicaTypeId : uint32_t
{
};
enum class ReplicationPeerStatus
{
	Waiting,
	Active,
	SchemaMismatch,
	ProtocolMismatch
};
/// Transitions observed on the receiving peer. An event holds no replica pointer.
enum class ReplicaEventKind
{
	EnteredScope,
	LeftScope,
	Destroyed,
	Released,
	Disconnected,
	AuthorityGranted,
	AuthorityCorrected,
	AuthorityLost,
	InputProcessed
};
struct OwnerInputStamp
{
	uint64_t epoch = 0, sequence = 0;
};
struct ReplicaEvent
{
	ReplicaEventKind kind;
	NetworkID id;
	PeerGuid source;
	OwnerInputStamp input{};
};
struct Packet;
struct ReplicationStatistics
{
	uint64_t desired = 0, constructed = 0, deferred = 0;
	uint64_t deferredState = 0;
	uint64_t enters = 0, leaves = 0, destroys = 0;
	uint64_t lifecycleBytes = 0, stateBytes = 0, clockBytes = 0;
	/// Owner reports from this peer dropped by SetOwnerUpdateRateLimit() before the accept callback.
	uint64_t ownerUpdatesDropped = 0;
};

enum class ReplicationTickMode
{
	Automatic,
	Manual
};

enum class ReplicationRole
{
	Unconfigured,
	Server,
	Client,
	Peer
};

/// Per-peer object replication. The service and its configuration outlive a
/// shutdown/startup cycle. Session objects and connections are cleared on shutdown.
class RAK_DLL_EXPORT ReplicationManager final
{
public:
	~ReplicationManager();
	ReplicationManager(const ReplicationManager &) = delete;
	ReplicationManager &operator=(const ReplicationManager &) = delete;

	void SetTickMode(ReplicationTickMode mode) { tickMode = mode; }
	void Tick();
	/// Drive replication at a strictly increasing authoritative simulation tick.
	bool Tick(Time simulationTick);
	/// Fractional tick for sampling a remote timeline. A client defaults to its server.
	/// Empty until two clock samples arrive. Delay is in addition to the measured jitter buffer.
	std::optional<double> GetRenderTick(unsigned interpolationDelayTicks = 2) const { return GetRenderTick(UNASSIGNED_PEER_GUID, interpolationDelayTicks); }
	std::optional<double> GetRenderTick(PeerGuid source, unsigned interpolationDelayTicks = 2) const;
	bool SetObserverStateBudget(PeerGuid observer, uint32_t bytesPerTick);
	bool SetUpdateInterval(PeerGuid observer, Replica3 &replica, unsigned ticks);

	/// Register the same numeric ID and codec version at both ends before connecting.
	template <class T>
	bool RegisterReplica(ReplicaTypeId id, uint16_t version)
	{
		return RegisterReplica<T>(id, version, []
		                          { return new T(); }, [](T *object)
		                          { delete object; });
	}
	template <class T, class Create, class Release>
	bool RegisterReplica(ReplicaTypeId id, uint16_t version, Create create, Release release)
	{
		constexpr bool snapshot = std::is_base_of_v<SnapshotReplicaTag, T>;
		bool timed = false;
		if constexpr (snapshot)
			timed = T::SnapshotHistoryCapacity != 0;
		return RegisterType(id, version, typeid(T), snapshot, timed, std::move(create),
		                    [release = std::move(release)](Replica3 *object) mutable
		                    { release(static_cast<T *>(object)); });
	}
	/// One upstream report format per replica type. The server decides which fields to apply.
	template <class T, class Report>
	bool RegisterOwnerUpdate(uint16_t version, bool (T::*accept)(PeerGuid, const Report &))
	{
		if (!accept)
			return false;
		return RegisterReport(typeid(T), typeid(Report), version, [accept](Replica3 &object, PeerGuid sender, BitStream &in)
		                      {
			Report report{};
			return report.Read(in) && !in.GetNumberOfUnreadBits() && (static_cast<T&>(object).*accept)(sender,report); });
	}
	/// Server: accept at most \a reportsPerSecond owner reports per replica of type \a id from its authority
	/// holder, with bursts of up to \a burst (zero means \a reportsPerSecond). Each grant starts with a full
	/// burst. Reports beyond it are dropped before the accept callback and counted in
	/// ReplicationStatistics::ownerUpdatesDropped; the client is not told. Zero, the default, accepts every
	/// report. A local policy, not part of the session schema: callable at any time.
	/// \return false when \a id has no registered owner report, or \a burst is set without a rate.
	bool SetOwnerUpdateRateLimit(ReplicaTypeId id, uint32_t reportsPerSecond, uint32_t burst = 0);
	bool SetSnapshotPolicy(ReplicaTypeId id, ChangeDetection detection, SnapshotDelivery delivery);
	bool MarkDirty(Replica3 &object);
	void MarkAllDirty();
	bool SetAuthority(Replica3 &replica, PeerGuid owner);
	bool RevokeAuthority(Replica3 &replica);
	bool CorrectAuthority(Replica3 &replica);
	bool HasLocalAuthority(Replica3 &replica) const;
	OwnerInputStamp GetLocalInputStamp(Replica3 &replica) const;
	template <class Report>
	bool SendOwnerUpdate(Replica3 &replica, const Report &report)
	{
		BitStream out;
		report.Write(out);
		return SendReport(replica, typeid(Report), out);
	}
	/// Request a canonical correction for predicted input. The stamp is written on success.
	template <class Report>
	bool SendOwnerUpdate(Replica3 &replica, const Report &report, OwnerInputStamp &sent)
	{
		BitStream out;
		report.Write(out);
		return SendReport(replica, typeid(Report), out, &sent);
	}
	Replica3 *FindReplica(NetworkID id) const;
	bool Destroy(Replica3 *replica);
	bool ReleaseRemote(NetworkID id);
	class RAK_DLL_EXPORT WorldGuard
	{
	public:
		explicit WorldGuard(ReplicationManager &manager);

	private:
		std::unique_lock<std::recursive_mutex> lock;
	};
	/// Current membership and cumulative traffic for active sessions. The default sums all sessions.
	ReplicationStatistics GetStatistics(PeerGuid guid = UNASSIGNED_PEER_GUID) const;
	ReplicationPeerStatus GetPeerStatus(PeerGuid guid) const;
	void SetAutomaticAdmission(bool enabled);
	bool Admit(PeerGuid guid);

	bool SetRole(ReplicationRole role);
	ReplicationRole GetRole() const { return role; }
	bool Reference(Replica3 *replica);
	bool Reference(Replica3 *replica, SpatialScope scope);
	InterestGrid &GetInterestGrid();
	bool ReferenceStatic(Replica3 *replica, NetworkID id);
	void Dereference(Replica3 *replica);
	unsigned int GetReplicaCount() const;
	unsigned int GetConnectionCount() const;
	Replica3 *GetReplicaAtIndex(unsigned int index) const;
	void GetLocalReplicas(std::vector<Replica3 *> &out) const;
	bool SetObserverVirtualWorld(PeerGuid guid, uint32_t dimension);
	void SetSerializeInterval(Time interval);
	void SetSerializeExecutor(RM3SerializeExecutor *executor);
	void SetSerializationList(std::function<bool(PeerGuid, std::vector<Replica3 *> &)> query);
	void SetTickCallbacks(std::function<void()> before, std::function<void()> after);
	void SetReplicaCallback(std::function<void(const ReplicaEvent &)> callback);
	void SetConnectionCallback(std::function<void(PeerGuid, bool)> callback);

	/// \name Delegation (server only: false, zero or no-op in any other role)
	/// The server hands each opted-in replica to the nearest eligible client to simulate, through SetAuthority, so the
	/// epoch, revocation and eligibility rules are the usual ones. Candidates are the Active, unsuspended radius observers
	/// of the interest grid that SetAuthority would accept for the replica. Each replica is re-elected about once per
	/// interval, staggered over ticks by its NetworkID, inside Tick after the interest rebuild; a disconnect re-elects
	/// what the client held (and what was pinned to it) at once, inside the disconnect, and a suspension, a pin or a grant
	/// MafiaNet revoked (an ineligible holder) at the next Tick. While a replica is delegated the election owns its authority: a SetAuthority or RevokeAuthority made by
	/// hand is re-elected at the next Tick. Use Pin, or opt out, to choose the holder yourself.
	/// @{
	/// Opts a replica in (a copy of \a policy is kept, normalised) or out (\a policy null: the current holder keeps it).
	/// \return false when the replica is not tracked in the interest grid, its type has no owner report, or it is static.
	bool SetDelegation(Replica3 &replica, const DelegationPolicy *policy);
	/// How often each delegated replica is re-elected. 500 by default; 0 is every tick.
	void SetDelegationInterval(uint32_t milliseconds);
	/// Total policy weight a client may hold, 0 for unlimited (the default). A client at its capacity keeps what it
	/// holds and takes nothing new. The per-client value overrides it until that client disconnects.
	void SetClientCapacity(uint32_t maxWeight);
	bool SetClientCapacity(PeerGuid guid, uint32_t maxWeight);
	/// A suspended client is elected for nothing and holds nothing the delegation assigns until Resume or its
	/// disconnect (the leader of a hand-owned group stays the game's grant; its followers are released). Takes effect at the
	/// next Tick, not inside the call: what it holds is re-elected then, without waiting for the interval, and the
	/// callback fires after that Tick (at most one Tick of delay; callbacks never fire from inside an API call).
	/// \return false for a guid without a session (Suspend) or that is not suspended (Resume).
	bool Suspend(PeerGuid guid);
	bool Resume(PeerGuid guid);
	bool IsSuspended(PeerGuid guid) const;
	/// Keeps a delegated replica on \a guid whatever the ranges say, from the next Tick; UNASSIGNED_PEER_GUID keeps it on
	/// the server (dormant). While the pinned client cannot hold it (suspended, ineligible) it is dormant. A pin ends
	/// with Unpin or the client's disconnect. Pin and Unpin take effect at the next Tick, not inside the call, whatever the
	/// interval; the callback fires after that Tick (at most one Tick of delay; callbacks never fire from inside an API
	/// call). On a group member it pins the whole group. \return false when the replica is not delegated and not in a
	/// group, when its group's root is not delegated (a hand-owned group cannot be pinned), or when \a guid has no
	/// session.
	bool Pin(Replica3 &replica, PeerGuid guid);
	bool Unpin(Replica3 &replica);
	bool IsPinned(Replica3 &replica) const;
	/// Called after a change of simulator is applied, outside every MafiaNet lock, in the order the changes were made:
	/// after the Tick that made it, or the disconnect. A pass asked for from inside it runs at the next Tick.
	void SetDelegationCallback(std::function<void(NetworkID, PeerGuid previous, PeerGuid current, DelegationChange)> callback);
	/// simulated and dormant: managed replicas (delegated ones, group members and the leaders of hand-owned groups)
	/// held by a client or by nobody, now.
	/// elections: replicas, or groups, evaluated. handovers: grant changes the delegation applied, every kind counted:
	/// an acquisition, a migration and a release to dormant each count one, for each replica of a group. A revocation MafiaNet or the game made outside the election does not count.
	/// refusals: elections the release veto postponed. forced: handovers made against the veto once the grace ran out.
	/// declines: declines the server recorded (withdrawals are not counted). Decline changes re-elect the replica at the
	/// next Tick, at most one such pass every 100 ms.
	DelegationStatistics GetDelegationStatistics() const;
	/// Summed weight of the managed replicas \a guid holds: the delegated ones (their policy's weight) and the members
	/// and leaders of delegation groups (their policy's weight if they have one, else 1).
	uint32_t GetDelegatedLoad(PeerGuid guid) const;
	/// Lets the game postpone a handover away from a holder: \a canRelease returns true when \a holder may give the
	/// replica up now. It is asked only when an election (a pin included) wants to move the replica away from a client
	/// holding it; never for a disconnect, a suspension or a holder that declined the replica. A refusal postpones the
	/// handover (counted in refusals, once per postponed election) for at most the policy's graceMs from the first
	/// refusal; after that the handover happens anyway (counted in forced). Null removes it.
	/// For a group it is asked once per handover, with the root leader's id and holder. It is called only from Tick's
	/// delegation pass (never from a disconnect), with MafiaNet's world lock held, unlike the delegation callback:
	/// answer from game state only. It must not call any ReplicationManager function that changes replication state (if it does, that election
	/// is void and redone at the next Tick), and must not wait on a lock that another thread can hold while calling
	/// into this ReplicationManager.
	void SetReleaseVeto(std::function<bool(NetworkID, PeerGuid holder)> canRelease);
	/// Links \a member to \a leader (null: unlinks it) so they are always simulated by the same client. The group is
	/// the tree under its root, the leader that follows nobody: links chain, and a link that would make a cycle is
	/// refused. Members always have the root's holder, whatever the root's mode. A delegated root is elected once for
	/// the group, with its policy and position, the group's summed weight, and only clients eligible for every member;
	/// the followers of a root the game owns by hand (not delegated, granted with SetAuthority) take its holder at the
	/// next Tick, or go dormant while that client cannot hold every one of them (ineligible, suspended); declines are
	/// ignored for such a group and its root's grant stays the game's. A member's own policy is kept but only its weight
	/// counts while grouped; a member without one weighs 1, and the leader of a hand-owned group counts toward its
	/// holder's load and capacity. Handovers are all or nothing: if any grant is refused, those already made in that
	/// pass are undone and the group is retried at its next pass. For an elected group (delegated root), Pin, Unpin and
	/// IsPinned on a member act on the group, a decline of any member declines the group, and suspending the holder
	/// releases the whole group; a hand-owned group cannot be pinned. The grid gets the matching
	/// InterestGrid::SetDependency(member, leader), so a client that sees the leader sees the member; eligibility is still
	/// checked per member. Unlinking clears that dependency only if it still names the old leader. Unlinking, or the
	/// leader's removal, leaves the member with its holder: it then follows its own policy, its own followers, or nothing
	/// (the game's again). \return false in any role but Server, when either replica
	/// is not one SetDelegation accepts, or for a cycle (\a leader is \a member or one of its followers).
	bool SetDelegationGroup(Replica3 &member, Replica3 *leader);
	/// @}

	/// Client: asks the server not to elect this client to simulate \a replica (\a declined true) or withdraws that,
	/// by a reliable message. The server passes a declining client over in its elections, takes the replica from it if
	/// it holds it (without asking the release veto), and forgets the decline when the replica leaves this client's
	/// visible set or either side disconnects. A pin overrides a decline without clearing it, and a decline of a member
	/// of an elected group (delegated root) declines the group. The server ignores it for a replica that is neither
	/// delegated nor in a delegation group, and it has no effect on a hand-owned group, which follows its root's holder. \return false in any role but Client, for a replica this client did not receive from the server, or when
	/// the message cannot be sent.
	bool DeclineAuthority(Replica3 &replica, bool declined);

private:
	friend class RakPeer;
	friend class ManagedReplication;
	explicit ReplicationManager(RakPeer &peer);
	unsigned BatchPayloadLimit(SystemIndex index) const;
	void Shutdown();

	bool RegisterType(ReplicaTypeId id, uint16_t version, std::type_index type, bool snapshot, bool timed, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release);
	bool RegisterReport(std::type_index object, std::type_index report, uint16_t version, std::function<bool(Replica3 &, PeerGuid, BitStream &)> accept);
	bool SendReport(Replica3 &object, std::type_index type, const BitStream &payload, OwnerInputStamp *sent = nullptr);
	ManagedReplication &Managed();

	bool ProcessPacket(Packet *packet);
	void DeliverDelegation();
	void ReceiveTick();
	RakPeer &peer;
	std::unique_ptr<NetworkIDManager> ids;
	std::unique_ptr<ManagedReplication> managed;
	ReplicationTickMode tickMode = ReplicationTickMode::Automatic;
	ReplicationRole role = ReplicationRole::Unconfigured;
};
}
