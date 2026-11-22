// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/replica_manager3.h"
#include "mafianet/replication_manager.h"
#include <map>
#include <set>
#include "replication_wire.h"

namespace MafiaNet
{
class ManagedReplication final : public ReplicaManager3
{
public:
	ManagedReplication(ReplicationManager &owner, RakPeerInterface &peer)
	    : ReplicaManager3(peer), owner(owner) {}
	bool Register(ReplicaTypeId id, uint16_t version, std::type_index type, bool snapshot, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release);
	bool Policy(ReplicaTypeId id, ChangeDetection detection, SnapshotDelivery delivery);
	bool Mark(Replica3 &object);
	void MarkAll();
	bool SetRegisteredType(Replica3 &replica) const;
	void Connected(PeerGuid guid, bool incoming);
	void Disconnected(PeerGuid guid);
	void Reset();
	bool Release(NetworkID id);
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
	void Tick();
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
	bool Submit(Replica3 &object, std::type_index type, const BitStream &payload);
	bool IsDownload(NetworkID id) const { return downloads.count(id) != 0; }
	bool HasSessions() const { return !sessions.empty(); }

private:
	class Connection;
	using Kind = ReplicationWire::Kind;
	struct Type
	{
		std::type_index cppType;
		uint16_t version;
		std::function<Replica3 *()> create;
		std::function<void(Replica3 *)> release;
		bool snapshot;
		ChangeDetection detection = ChangeDetection::Compare;
		SnapshotDelivery delivery = SnapshotDelivery::Reliable;
	};
	struct Session
	{
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
	void ReadState(Connection &connection, StateDelivery &delivery, BitStream &in, bool acknowledge);
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
		bool ready = false;
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
