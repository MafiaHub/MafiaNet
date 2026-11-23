// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/replica_manager3.h"
#include "mafianet/replication_manager.h"
#include <map>
#include <mutex>
#include <set>
#include "replication_wire.h"
#include "replication_clock.h"

namespace MafiaNet
{
class ManagedReplication final : public ReplicaManager3
{
public:
	ManagedReplication(ReplicationManager &owner, RakPeerInterface &peer)
	    : ReplicaManager3(peer), owner(owner) {}
	bool Register(ReplicaTypeId id, uint16_t version, std::type_index type, bool snapshot, bool timed, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release);
	bool Policy(ReplicaTypeId id, ChangeDetection detection, SnapshotDelivery delivery);
	bool Mark(Replica3 &object);
	void MarkAll();
	bool SetRegisteredType(Replica3 &replica) const;
	void Connected(PeerGuid guid, const SystemAddress &address, bool incoming);
	void Disconnected(PeerGuid guid);
	void Reset();
	bool Release(NetworkID id, ReplicaEventKind reason = ReplicaEventKind::Released);
	void DispatchEvents();
	using ReplicaCallback = std::function<void(const ReplicaEvent &)>;
	std::shared_ptr<ReplicaCallback> replicaChanged;
	bool Admit(PeerGuid guid);
	ReplicationPeerStatus Status(PeerGuid guid) const;
	void Receive(Packet &packet);
	void Destroy(Replica3 &replica);
	InterestGrid interest;
	std::map<NetworkID, Replica3 *> ordinary;
	void OnFinalDereference(Replica3 &replica) override;
	std::map<NetworkID, Replica3 *> staticObjects;
	bool automaticAdmission = true;
	std::function<bool(PeerGuid, std::vector<Replica3 *> &)> serializationList;
	std::function<void()> beforeTick, afterTick;
	std::function<void(PeerGuid, bool)> connectionChanged;
	bool Tick(Time requested = 0);
	std::optional<double> RenderTick(PeerGuid source, unsigned delay) const;
	bool ConfigureObserver(PeerGuid observer, Replica3 *object, unsigned value);
	struct Report
	{
		std::type_index cppType;
		uint16_t version;
		std::function<bool(Replica3 &, PeerGuid, BitStream &)> accept;
	};
	ReplicationStatistics Statistics(PeerGuid guid);
	bool RegisterReport(std::type_index object, Report report);
	bool Assign(Replica3 &object, PeerGuid guid, bool force = false);
	bool Correct(Replica3 &object);
	bool Owns(Replica3 &object) const;
	OwnerInputStamp LocalStamp(Replica3 &object) const;
	bool Submit(Replica3 &object, std::type_index type, const BitStream &payload, OwnerInputStamp *sent);
	bool IsDownload(NetworkID id) const { return downloads.count(id) != 0; }
	bool HasSessions() const { return !sessions.empty(); }

private:
	class Connection;
	void Notify(ReplicaEventKind kind, NetworkID id, PeerGuid source, OwnerInputStamp input = {});
	std::deque<ReplicaEvent> events;
	bool dispatching = false;
	using Kind = ReplicationWire::Kind;
	struct Type
	{
		std::type_index cppType;
		uint16_t version;
		std::function<Replica3 *()> create;
		std::function<void(Replica3 *)> release;
		bool snapshot, timed;
		ChangeDetection detection = ChangeDetection::Compare;
		SnapshotDelivery delivery = SnapshotDelivery::Reliable;
	};
	struct Session
	{
		SystemAddress address;
		ReplicationPeerStatus status = ReplicationPeerStatus::Waiting;
		uint64_t clientToken = 0, serverToken = 0;
		bool initiator = false;
		bool admitted = false, helloReceived = false;
	};
	Connection_RM3 *AllocConnection(const SystemAddress &, RakNetGUID) const override { return nullptr; }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }
	uint64_t Schema() const;
	void Handshake(PeerGuid guid, Session &session, Kind kind);
	void RetryControl();
	void PublishClock();
	bool hasTimedTypes = false;
	TimeUS nextClockSample = 0;
	// Render and serialization callbacks may query the clock while holding a connection lock.
	mutable std::mutex clockMutex;
	std::map<PeerGuid, ReplicationClock> clocks;
	PeerGuid clockServer = UNASSIGNED_PEER_GUID;
	std::map<PeerGuid, Kind> pendingHandshakes;
	struct Revocation
	{
		PeerGuid holder;
		NetworkID id;
		uint64_t epoch;
	};
	std::vector<Revocation> pendingRevocations;
	bool SendRevoke(const Revocation &revocation);
	Connection *Activate(PeerGuid guid, Session &session);
	bool Send(PeerGuid guid, BitStream &packet);
	void WriteHeader(BitStream &out, Kind kind, const Session &session) const;
	void ReadState(Connection &connection, StateDelivery &delivery, BitStream &in, ReplicationWire::SnapshotAcks *acks, uint64_t seq = 0, Time timestamp = 0);
	void Apply(Connection &connection, Kind kind, BitStream &in);

	ReplicationManager &owner;
	std::map<ReplicaTypeId, Type> types;
	bool explicitScheduling = false;
	std::map<std::type_index, ReplicaTypeId> typeIds;
	std::map<PeerGuid, Session> sessions;
	struct Download
	{
		ReplicaTypeId type;
		PeerGuid source;
	};
	std::map<NetworkID, Download> downloads;
	struct Grant
	{
		PeerGuid holder;
		uint64_t epoch;
		uint64_t sequence = 0;
		uint64_t publishedEpoch = 0;
		uint64_t requestedFeedback = 0, publishedFeedback = 0;
		bool stateChanged = false;
		bool Ready() const { return publishedEpoch == epoch; }
	};
	std::map<ReplicaTypeId, Report> reports;
	std::map<NetworkID, Grant> grants;
	uint64_t nextEpoch = 0;
	void UpdateGrants(bool publish);
	void OwnerPacket(Connection &connection, Kind kind, BitStream &in);
	void FinishTick();
	struct TickScope
	{
		ManagedReplication &manager;
		~TickScope() { manager.FinishTick(); }
	};
	std::set<PeerGuid> closing;
	bool resetRequested = false;
	bool ticking = false;
	uint64_t generation = 0;
};
}
