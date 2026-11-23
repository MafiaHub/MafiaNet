// Copyright (c) 2026, MafiaHub. MIT license.

#pragma once

#include "mafianet/export.h"
#include "mafianet/bit_stream.h"
#include "mafianet/interest_grid.h"

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
	void ReceiveTick();
	RakPeer &peer;
	std::unique_ptr<NetworkIDManager> ids;
	std::unique_ptr<ManagedReplication> managed;
	ReplicationTickMode tickMode = ReplicationTickMode::Automatic;
	ReplicationRole role = ReplicationRole::Unconfigured;
};
}
