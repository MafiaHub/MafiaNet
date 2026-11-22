// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/replica_manager3.h"
#include "mafianet/replication_manager.h"
#include <map>
#include "replication_wire.h"

namespace MafiaNet
{
class ManagedReplication final : public ReplicaManager3
{
public:
	ManagedReplication(ReplicationManager &owner, RakPeerInterface &peer)
	    : ReplicaManager3(peer), owner(owner) {}
	bool Register(ReplicaTypeId id, uint16_t version, std::type_index type, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release);
	bool SetRegisteredType(Replica3 &replica) const;
	void Connected(PeerGuid guid, bool incoming);
	void Disconnected(PeerGuid guid);
	void Reset();
	bool Release(NetworkID id);
	bool Admit(PeerGuid guid);
	ReplicationPeerStatus Status(PeerGuid guid) const;
	void Receive(Packet &packet);
	void Destroy(Replica3 &replica);
	std::map<NetworkID, Replica3 *> staticObjects;
	bool automaticAdmission = true;
	std::function<bool(PeerGuid, std::vector<Replica3 *> &)> serializationList;
	std::function<void()> beforeTick, afterTick;
	std::function<void(PeerGuid, bool)> connectionChanged;
	void Tick();
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
	void Start(PeerGuid guid, Session &session);
	Connection *Activate(PeerGuid guid, Session &session);
	void Send(PeerGuid guid, BitStream &packet);
	void WriteHeader(BitStream &out, Kind kind, const Session &session) const;
	void Apply(Connection &connection, Kind kind, BitStream &in);

	ReplicationManager &owner;
	std::map<ReplicaTypeId, Type> types;
	std::map<std::type_index, ReplicaTypeId> typeIds;
	std::map<PeerGuid, Session> sessions;
	struct Download
	{
		ReplicaTypeId type;
		PeerGuid source;
	};
	std::map<NetworkID, Download> downloads;
	bool ticking = false;
	uint64_t generation = 0;
};
}
