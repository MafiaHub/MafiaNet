// Copyright (c) 2026, MafiaHub. MIT license.
#include "managed_replication.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"
#include "mafianet/get_time.h"
#include <array>
#include <limits>
#include <sodium.h>

namespace MafiaNet
{
using namespace ReplicationWire;
class ManagedReplication::Connection final : public Connection_RM3
{
public:
	Connection(ManagedReplication &runtime, PeerGuid guid, const Session &session)
	    : Connection_RM3(runtime.GetRakPeerInterface()->GetSystemAddressFromGuid(ToGuid(guid)), ToGuid(guid)), runtime(runtime)
	{
		clientToken = session.clientToken;
		serverToken = session.serverToken;
	}
	struct Visit
	{
		uint64_t incarnation;
		uint32_t referenceIndex;
		std::array<uint64_t, RM3_NUM_OUTPUT_BITSTREAM_CHANNELS> lastState{};
	};
	std::map<NetworkID, Visit> incoming;
	bool Matches(uint64_t client, uint64_t server) const { return clientToken == client && serverToken == server; }
	ConstructionMode QueryConstructionMode() const override { return QUERY_CONNECTION_FOR_REPLICA_LIST; }
	bool QuerySerializationList(std::vector<Replica3 *> &out) override
	{
		if (runtime.serializationList && runtime.serializationList(ToPeerGuid(guid), out))
			return true;
		if (!runtime.explicitScheduling)
			return false;
		for (auto id : polled)
			if (auto *object = runtime.owner.FindReplica(id))
				out.push_back(object);
		for (auto id : dirty)
			if (auto *object = runtime.owner.FindReplica(id))
				out.push_back(object);
		dirty.clear();
		return true;
	}
	void QueryReplicaList(std::vector<Replica3 *> &enter, std::vector<Replica3 *> &leave) override
	{
		if (runtime.owner.GetRole() == ReplicationRole::Client)
			return;
		const auto observer = ToPeerGuid(guid);
		std::vector<NetworkID> desired;
		runtime.interest.GetVisible(observer, desired);
		for (const auto &[id, object] : runtime.ordinary)
			if (!runtime.interest.Contains(id) && runtime.interest.Eligible(observer, id))
				desired.push_back(id);

		std::set<NetworkID> wanted(desired.begin(), desired.end());
		std::vector<Replica3 *> constructed;
		GetConstructedReplicas(constructed);
		for (auto *object : constructed)
			if (!wanted.count(object->GetNetworkID()) || object->QueryDestruction(this, &runtime) == RM3DS_SEND_DESTRUCTION)
				leave.push_back(object);

		for (auto *object : leave)
			Forget(object->GetNetworkID());
		FlushUnconfirmed();
		++tick;
		for (auto it = pending.begin(); it != pending.end();)
		{
			if (!wanted.count(it->first))
				it = pending.erase(it);
			else
				++it;
		}
		for (const auto id : desired)
		{
			auto *object = runtime.owner.FindReplica(id);
			if (!object || object->creatingSystemGUID != runtime.GetRakPeerInterface()->GetMyGUID() || HasReplicaConstructed(object))
				continue;
			const auto construction = object->QueryConstruction(this, &runtime);
			if (construction == RM3CS_SEND_CONSTRUCTION || construction == RM3CS_ALREADY_EXISTS_REMOTELY)
			{
				pending.try_emplace(id, tick);
				enter.push_back(object);
			}
		}
		const auto budget = runtime.interest.GetBudget(observer);
		if (budget && enter.size() > budget)
		{
			std::stable_sort(enter.begin(), enter.end(), [&](Replica3 *a, Replica3 *b)
			                 { return pending.at(a->GetNetworkID()) < pending.at(b->GetNetworkID()); });
			enter.resize(budget);
		}
		for (auto *object : enter)
		{
			const auto id = object->GetNetworkID();
			pending.erase(id);
			if (runtime.explicitScheduling && runtime.types.at(static_cast<ReplicaTypeId>(object->registeredType)).detection == ChangeDetection::Compare)
				polled.insert(id);
		}
	}

	std::set<NetworkID> dirty, polled;
	struct Unconfirmed
	{
		uint64_t seq;
		TimeMS sent;
		BitSize_t bits;
		std::vector<unsigned char> bytes;
	};
	std::map<NetworkID, Unconfirmed> unconfirmed;
	void Forget(NetworkID id)
	{
		dirty.erase(id);
		polled.erase(id);
		unconfirmed.erase(id);
		pending.erase(id);
	}
	void TransmitState(Replica3 &object, BitStream &out, PRO pro, RakPeerInterface &peer) override
	{
		if (!object.replaceableSnapshot)
		{
			Connection_RM3::TransmitState(object, out, pro, peer);
			return;
		}
		auto &item = unconfirmed[object.GetNetworkID()];
		item.seq = nextState;
		item.sent = GetTimeMS();
		item.bits = out.GetNumberOfBitsUsed();
		item.bytes.assign(out.GetData(), out.GetData() + out.GetNumberOfBytesUsed());
		peer.Send(&out, pro.priority, Reliability::Unreliable, 0, guid, false);
	}
	void FlushUnconfirmed()
	{
		const auto now = GetTimeMS();
		for (auto it = unconfirmed.begin(); it != unconfirmed.end();)
		{
			auto &item = it->second;
			if (now - item.sent < 250)
			{
				++it;
				continue;
			}
			BitStream retry(item.bytes.data(), static_cast<unsigned>(item.bytes.size()), false);
			retry.SetWriteOffset(item.bits);
			if (runtime.GetRakPeerInterface()->Send(&retry, Priority::High, Reliability::ReliableOrdered, 0, guid, false))
				it = unconfirmed.erase(it);
			else
				++it;
		}
	}
	void Acknowledge(NetworkID id, uint64_t seq)
	{
		BitStream ack;
		ReplicationWire::WriteHeader(ack, Kind::SnapshotAck, clientToken, serverToken);
		ack.Write(id);
		ack.Write(seq);
		runtime.GetRakPeerInterface()->Send(&ack, Priority::High, Reliability::Unreliable, 0, guid, false);
	}

private:
	uint64_t tick = 0;
	std::map<NetworkID, uint64_t> pending;
	ManagedReplication &runtime;
};

bool ManagedReplication::Register(ReplicaTypeId id, uint16_t version, std::type_index type, bool snapshot, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release)
{
	if (!sessions.empty() || !create || !release || types.count(id) || typeIds.count(type))
		return false;
	types.emplace(id, Type{type, version, std::move(create), std::move(release), snapshot});
	typeIds.emplace(type, id);
	return true;
}

bool ManagedReplication::Policy(ReplicaTypeId id, ChangeDetection detection, SnapshotDelivery delivery)
{
	auto it = types.find(id);
	if (HasSessions() || GetReplicaCount() || it == types.end() || !it->second.snapshot)
		return false;
	it->second.detection = detection;
	it->second.delivery = delivery;
	explicitScheduling = std::any_of(types.begin(), types.end(), [](const auto &entry)
	                                 { return entry.second.detection == ChangeDetection::Explicit; });
	return true;
}
bool ManagedReplication::Mark(Replica3 &object)
{
	const auto id = object.GetNetworkID();
	if (owner.FindReplica(id) != &object || object.creatingSystemGUID != GetRakPeerInterface()->GetMyGUID() || types.at(static_cast<ReplicaTypeId>(object.registeredType)).detection != ChangeDetection::Explicit)
		return false;
	for (unsigned i = 0; i < GetConnectionCount(); ++i)
	{
		auto *c = static_cast<Connection *>(GetConnectionAtIndex(i));
		if (c->HasReplicaConstructed(&object))
			c->dirty.insert(id);
	}
	return true;
}
void ManagedReplication::MarkAll()
{
	for (unsigned i = 0; i < GetReplicaCount(); ++i)
		Mark(*GetReplicaAtIndex(i));
}
bool ManagedReplication::SetRegisteredType(Replica3 &replica) const
{
	auto it = typeIds.find(typeid(replica));
	if (it == typeIds.end())
		return false;
	replica.replaceableSnapshot = types.at(it->second).delivery == SnapshotDelivery::Replaceable;
	replica.registeredType = static_cast<uint32_t>(it->second);
	return true;
}

uint64_t ManagedReplication::Schema() const
{
	// Stable across registration order, ABI and host endianness. Game versions cover codec semantics.
	uint64_t hash = 14695981039346656037ull;
	for (const auto &entry : types)
	{
		uint64_t word = (static_cast<uint64_t>(entry.first) << 16) | entry.second.version;
		for (unsigned int i = 0; i < 6; ++i)
		{
			hash ^= word & 255;
			hash *= 1099511628211ull;
			word >>= 8;
		}
	}
	for (const auto &[id, report] : reports)
	{
		uint64_t word = (static_cast<uint64_t>(id) << 16) | report.version;
		hash ^= 0xff;
		hash *= 1099511628211ull;
		for (unsigned i = 0; i < 6; ++i)
		{
			hash ^= word & 255;
			hash *= 1099511628211ull;
			word >>= 8;
		}
	}
	for (const auto &[id, type] : types)
	{
		(void) id;
		hash ^= static_cast<uint64_t>(type.delivery);
		hash *= 1099511628211ull;
	}
	hash ^= owner.GetRole() == ReplicationRole::Peer ? 1 : 0;
	return hash;
}
void ManagedReplication::WriteHeader(BitStream &out, Kind kind, const Session &session) const
{
	ReplicationWire::WriteHeader(out, kind, session.clientToken, session.serverToken);
}
void ManagedReplication::Send(PeerGuid guid, BitStream &packet)
{
	GetRakPeerInterface()->Send(&packet, Priority::High, Reliability::ReliableOrdered, 0, ToGuid(guid), false);
}
void ManagedReplication::Connected(PeerGuid guid, bool incoming)
{
	if (owner.GetRole() != ReplicationRole::Peer && ((owner.GetRole() == ReplicationRole::Client) == incoming || (owner.GetRole() == ReplicationRole::Client && !sessions.empty())))
		return;
	auto inserted = sessions.try_emplace(guid);
	if (inserted.second)
	{
		inserted.first->second.initiator = !incoming;
		if (automaticAdmission)
			Admit(guid);
	}
}
bool ManagedReplication::Admit(PeerGuid guid)
{
	auto it = sessions.find(guid);
	if (it == sessions.end() || it->second.admitted)
		return false;
	auto &session = it->second;
	session.admitted = true;
	if (session.initiator)
	{
		randombytes_buf(&session.clientToken, sizeof(session.clientToken));
		if (!session.clientToken)
			session.clientToken = 1;
		BitStream out;
		WriteHeader(out, Kind::Hello, session);
		out.Write(Schema());
		Send(guid, out);
	}
	else if (session.helloReceived && session.status == ReplicationPeerStatus::Waiting)
		Start(guid, session);
	return true;
}
ReplicationPeerStatus ManagedReplication::Status(PeerGuid guid) const
{
	auto it = sessions.find(guid);
	return it == sessions.end() ? ReplicationPeerStatus::Waiting : it->second.status;
}
ManagedReplication::Connection *ManagedReplication::Activate(PeerGuid guid, Session &session)
{
	auto connection = std::make_unique<Connection>(*this, guid, session);
	if (!PushConnection(connection.get()))
		return nullptr;
	session.status = ReplicationPeerStatus::Active;
	return connection.release();
}
void ManagedReplication::Start(PeerGuid guid, Session &session)
{
	randombytes_buf(&session.serverToken, sizeof(session.serverToken));
	if (!session.serverToken)
		session.serverToken = 1;
	if (!Activate(guid, session))
		return;
	BitStream out;
	WriteHeader(out, Kind::Accept, session);
	out.Write(Schema());
	Send(guid, out);
	if (connectionChanged)
		connectionChanged(guid, true);
}
bool ManagedReplication::Release(NetworkID id)
{
	auto it = downloads.find(id);
	if (it == downloads.end())
		return false;
	const auto type = it->second.type;
	{
		StateDelivery guard(*this, ToGuid(it->second.source));
		if (auto *conn = static_cast<Connection *>(guard.GetConnection()))
			conn->incoming.erase(id);
	}
	downloads.erase(it);
	auto *replica = GetNetworkIDManager()->GET_BASE_OBJECT_FROM_ID(id);
	if (replica)
	{
		auto *object = static_cast<Replica3 *>(replica);
		Dereference(object);
		types.at(type).release(object);
	}
	return true;
}
void ManagedReplication::Tick()
{
	WorldGuard lock(*this);
	if (ticking)
		return;
	ticking = true;
	TickScope scope{*this};
	if (beforeTick)
		beforeTick();
	if (resetRequested)
		return;
	UpdateGrants(false);
	Update();
	if (resetRequested)
		return;
	UpdateGrants(true);
	if (afterTick)
		afterTick();
}
void ManagedReplication::FinishTick()
{
	ticking = false;
	if (resetRequested)
	{
		resetRequested = false;
		closing.clear();
		Reset();
		return;
	}
	auto closed = std::move(closing);
	closing.clear();
	for (auto guid : closed)
		Disconnected(guid);
}
void ManagedReplication::Disconnected(PeerGuid guid)
{
	if (ticking)
	{
		closing.insert(guid);
		return;
	}
	auto it = sessions.find(guid);
	if (it == sessions.end())
		return;
	const bool active = it->second.status == ReplicationPeerStatus::Active;
	sessions.erase(it);
	interest.RemoveObserver(guid);
	for (auto grant = grants.begin(); grant != grants.end();)
	{
		if (grant->second.holder == guid)
			grant = grants.erase(grant);
		else
			++grant;
	}
	++generation;
	std::vector<NetworkID> remove;
	for (const auto &entry : downloads)
		if (entry.second.source == guid)
			remove.push_back(entry.first);
	for (auto id : remove)
		Release(id);
	delete PopConnection(ToGuid(guid));
	if (active && connectionChanged)
		connectionChanged(guid, false);
}
void ManagedReplication::OnFinalDereference(Replica3 &replica)
{
	const auto id = replica.GetNetworkID();
	for (unsigned i = 0; i < GetConnectionCount(); ++i)
	{
		auto guid = GetConnectionAtIndex(i)->GetRakNetGUID();
		StateDelivery lock(*this, guid);
		if (auto *connection = static_cast<Connection *>(lock.GetConnection()))
			connection->Forget(id);
	}
	ordinary.erase(id);
	grants.erase(id);
	staticObjects.erase(id);
	interest.Remove(id);
}
void ManagedReplication::Reset()
{
	WorldGuard guard(*this);
	if (ticking)
	{
		resetRequested = true;
		return;
	}
	++generation;
	while (!sessions.empty())
		Disconnected(sessions.begin()->first);
	grants.clear();
	staticObjects.clear();
	ordinary.clear();
	interest.Clear();
	Clear(false);
}
void ManagedReplication::Destroy(Replica3 &replica)
{
	for (unsigned int i = 0; i < GetConnectionCount(); ++i)
	{
		auto *connection = static_cast<Connection *>(GetConnectionAtIndex(i));
		if (connection->HasReplicaConstructed(&replica))
			connection->SendRemoval(replica, true, *GetRakPeerInterface(), 0);
	}
	Dereference(&replica);
}

void ManagedReplication::Receive(Packet &packet)
{
	const auto guid = ToPeerGuid(packet.guid);
	if (packet.length > (MaxPayloadBits / 8) * RM3_NUM_OUTPUT_BITSTREAM_CHANNELS + 256 ||
	    packet.bitSize > static_cast<uint64_t>(packet.length) * 8)
		return;
	BitStream in(packet.data, packet.length, false);
	in.SetWriteOffset(packet.bitSize);
	in.IgnoreBytes(1);
	uint8_t version;
	Kind kind;
	WorldId world;
	uint64_t clientToken, serverToken;
	if (!in.Read(version) || !in.Read(kind) || !in.Read(clientToken) || !in.Read(serverToken) || !in.Read(world) || world != 0 || !clientToken)
		return;
	if (kind == Kind::State || kind == Kind::Snapshot || kind == Kind::SnapshotAck)
	{
		if (version != Version || (kind != Kind::SnapshotAck && owner.GetRole() == ReplicationRole::Server))
			return;
		StateDelivery delivery(*this, packet.guid);
		auto *connection = static_cast<Connection *>(delivery.GetConnection());
		if (!connection || !connection->Matches(clientToken, serverToken))
			return;
		if (kind == Kind::SnapshotAck)
		{
			NetworkID id;
			uint64_t seq;
			if (!in.Read(id) || !in.Read(seq) || in.GetNumberOfUnreadBits())
				return;
			auto it = connection->unconfirmed.find(id);
			if (it != connection->unconfirmed.end() && it->second.seq == seq)
				connection->unconfirmed.erase(it);
		}
		else
			ReadState(*connection, delivery, in, kind == Kind::Snapshot);
		return;
	}
	WorldGuard guard(*this);
	auto it = sessions.find(guid);
	if (it == sessions.end())
		return;
	auto &session = it->second;
	const bool server = !session.initiator;
	if (kind == Kind::Hello && server && session.status == ReplicationPeerStatus::Waiting && !session.helloReceived && !serverToken)
	{
		uint64_t schema;
		if (!in.Read(schema) || in.GetNumberOfUnreadBits())
			return;
		session.clientToken = clientToken;
		session.helloReceived = true;
		if (version != Version || schema != Schema())
		{
			session.status = version != Version ? ReplicationPeerStatus::ProtocolMismatch : ReplicationPeerStatus::SchemaMismatch;
			BitStream out;
			WriteHeader(out, Kind::Reject, session);
			out.Write(static_cast<uint8_t>(session.status));
			Send(guid, out);
		}
		else if (session.admitted)
			Start(guid, session);
		return;
	}
	if (version != Version || clientToken != session.clientToken || !session.admitted)
		return;
	if (!server && session.status == ReplicationPeerStatus::Waiting)
	{
		if (kind == Kind::Accept)
		{
			uint64_t schema;
			if (serverToken && in.Read(schema) && schema == Schema() && !in.GetNumberOfUnreadBits())
			{
				session.serverToken = serverToken;
				if (Activate(guid, session) && connectionChanged)
					connectionChanged(guid, true);
			}
		}
		else if (kind == Kind::Reject)
		{
			uint8_t reason;
			if (in.Read(reason) && !in.GetNumberOfUnreadBits() &&
			    (reason == static_cast<uint8_t>(ReplicationPeerStatus::SchemaMismatch) || reason == static_cast<uint8_t>(ReplicationPeerStatus::ProtocolMismatch)))
				session.status = static_cast<ReplicationPeerStatus>(reason);
		}
		return;
	}
	if (session.status != ReplicationPeerStatus::Active || session.serverToken != serverToken)
		return;
	StateDelivery delivery(*this, packet.guid);
	auto *conn = static_cast<Connection *>(delivery.GetConnection());
	if (!conn)
		return;
	if (kind == Kind::OwnerUpdate || kind == Kind::Grant || kind == Kind::Revoke)
		OwnerPacket(*conn, kind, in);
	else if (owner.GetRole() == ReplicationRole::Peer || !server)
		Apply(*conn, kind, in);
}

bool ManagedReplication::RegisterReport(std::type_index object, Report report)
{
	auto it = typeIds.find(object);
	return !HasSessions() && it != typeIds.end() && reports.emplace(it->second, std::move(report)).second;
}

bool ManagedReplication::Assign(Replica3 &object, PeerGuid guid, bool force)
{
	const auto id = object.GetNetworkID();
	if (owner.GetRole() != ReplicationRole::Server || owner.FindReplica(id) != &object || !reports.count(static_cast<ReplicaTypeId>(object.registeredType)))
		return false;
	if (guid != UNASSIGNED_PEER_GUID && (Status(guid) != ReplicationPeerStatus::Active || !interest.Eligible(guid, id)))
		return false;
	auto current = grants.find(id);
	if (current != grants.end())
	{
		if (current->second.holder == guid && !force)
			return true;
		if (current->second.ready)
		{
			BitStream revoke;
			WriteHeader(revoke, Kind::Revoke, sessions.at(current->second.holder));
			revoke.Write(id);
			revoke.Write(current->second.epoch);
			Send(current->second.holder, revoke);
		}
		grants.erase(current);
	}
	else if (guid == UNASSIGNED_PEER_GUID)
		return false;
	interest.Require(id, guid);
	if (guid != UNASSIGNED_PEER_GUID)
		grants.emplace(id, Grant{guid, ++nextEpoch});
	return true;
}

bool ManagedReplication::Correct(Replica3 &object)
{
	auto it = grants.find(object.GetNetworkID());
	return it != grants.end() && Assign(object, it->second.holder, true);
}

bool ManagedReplication::Owns(Replica3 &object) const
{
	const auto id = object.GetNetworkID();
	return owner.GetRole() == ReplicationRole::Client && owner.FindReplica(id) == &object && grants.count(id);
}

bool ManagedReplication::Submit(Replica3 &object, std::type_index type, const BitStream &payload)
{
	if (!Owns(object))
		return false;
	auto codec = reports.find(static_cast<ReplicaTypeId>(object.registeredType));
	if (codec == reports.end() || codec->second.cppType != type)
		return false;
	const auto id = object.GetNetworkID();
	auto &grant = grants.at(id);
	BitStream out;
	WriteHeader(out, Kind::OwnerUpdate, sessions.at(grant.holder));
	out.Write(id);
	out.Write(grant.epoch);
	out.Write(++grant.sequence);
	if (!WriteBlock(out, payload))
		return false;
	Send(grant.holder, out);
	return true;
}

void ManagedReplication::UpdateGrants(bool publish)
{
	if (owner.GetRole() != ReplicationRole::Server)
		return;
	std::vector<NetworkID> ids;
	for (const auto &entry : grants)
		ids.push_back(entry.first);
	for (auto id : ids)
	{
		auto it = grants.find(id);
		if (it == grants.end())
			continue;
		const auto grant = it->second;
		auto *object = owner.FindReplica(id);
		auto *connection = GetConnectionByGUID(ToGuid(grant.holder));
		if (!object || !connection)
			continue;
		if (!interest.Eligible(grant.holder, id) || (grant.ready && !connection->HasReplicaConstructed(object)))
		{
			Assign(*object, UNASSIGNED_PEER_GUID);
			continue;
		}
		if (!publish || grant.ready || !connection->HasReplicaConstructed(object))
			continue;
		BitStream snapshot, out;
		object->SerializeConstruction(&snapshot, connection);
		it = grants.find(id);
		if (it == grants.end() || it->second.epoch != grant.epoch)
			continue;
		WriteHeader(out, Kind::Grant, sessions.at(grant.holder));
		out.Write(id);
		out.Write(grant.epoch);
		if (!WriteBlock(out, snapshot))
			continue;
		Send(grant.holder, out);
		it->second.ready = true;
	}
}

void ManagedReplication::OwnerPacket(Connection &connection, Kind kind, BitStream &in)
{
	NetworkID id;
	uint64_t epoch;
	if (!in.Read(id) || !in.Read(epoch) || !epoch)
		return;
	auto *object = owner.FindReplica(id);
	if (!object)
		return;
	const auto sender = ToPeerGuid(connection.GetRakNetGUID());
	auto it = grants.find(id);
	if (kind == Kind::OwnerUpdate)
	{
		if (owner.GetRole() != ReplicationRole::Server || it == grants.end() || it->second.holder != sender || it->second.epoch != epoch || !it->second.ready || !interest.Eligible(sender, id) || !connection.HasReplicaConstructed(object))
			return;
		uint64_t seq;
		BitStream payload;
		if (!in.Read(seq) || !seq || seq <= it->second.sequence || !ReadBlock(in, payload) || in.GetNumberOfUnreadBits())
			return;
		it->second.sequence = seq;
		reports.at(static_cast<ReplicaTypeId>(object->registeredType)).accept(*object, sender, payload);
		return;
	}
	if (owner.GetRole() != ReplicationRole::Client || !downloads.count(id) || downloads.at(id).source != sender)
		return;
	if (kind == Kind::Revoke)
	{
		if (!in.GetNumberOfUnreadBits() && it != grants.end() && it->second.epoch == epoch)
			grants.erase(it);
		return;
	}
	BitStream snapshot;
	if (!ReadBlock(in, snapshot) || in.GetNumberOfUnreadBits() || !reports.count(static_cast<ReplicaTypeId>(object->registeredType)))
		return;
	const auto before = generation;
	if (object->DeserializeConstruction(&snapshot, &connection) && generation == before && owner.FindReplica(id) == object)
		grants.insert_or_assign(id, Grant{sender, epoch, 0, true});
}

void ManagedReplication::Apply(Connection &connection, Kind kind, BitStream &in)
{
	NetworkID id;
	uint64_t visit;
	if (!in.Read(id) || id == UNASSIGNED_NETWORK_ID || !in.Read(visit) || !visit)
		return;
	if (kind == Kind::Leave || kind == Kind::Destroy)
	{
		auto it = connection.incoming.find(id);
		if (in.GetNumberOfUnreadBits() || it == connection.incoming.end() || it->second.incarnation != visit)
			return;
		connection.incoming.erase(it);
		Release(id);
		return;
	}
	auto *replica = static_cast<Replica3 *>(GetNetworkIDManager()->GET_BASE_OBJECT_FROM_ID(id));
	if ((kind == Kind::Spawn && !replica) || (kind == Kind::Bind && replica && staticObjects.count(id)))
	{
		ReplicaTypeId type;
		BitStream snapshot;
		if (!in.Read(type) || !ReadBlock(in, snapshot) || in.GetNumberOfUnreadBits())
			return;
		auto entry = types.find(type);
		if (entry == types.end())
			return;
		if (kind == Kind::Bind)
		{
			if (entry->second.cppType == typeid(*replica) && replica->QueryRemoteConstruction(&connection))
			{
				connection.incoming.insert_or_assign(id, Connection::Visit{visit, replica->referenceIndex});
				replica->DeserializeConstructionExisting(&snapshot, &connection);
			}
			return;
		}
		const auto before = generation;
		auto *copy = entry->second.create();
		if (!copy)
			return;
		if (generation != before || !copy->QueryRemoteConstruction(&connection) || generation != before ||
		    !copy->DeserializeConstruction(&snapshot, &connection) || generation != before)
		{
			entry->second.release(copy);
			return;
		}
		copy->registeredType = static_cast<uint32_t>(type);
		copy->creatingSystemGUID = connection.GetRakNetGUID();
		copy->SetNetworkID(id);
		Reference(copy);
		connection.incoming.insert_or_assign(id, Connection::Visit{visit, copy->referenceIndex});
		downloads.emplace(id, Download{type, ToPeerGuid(connection.GetRakNetGUID())});
	}
}

void ManagedReplication::ReadState(Connection &connection, StateDelivery &delivery, BitStream &in, bool acknowledge)
{
	NetworkID id;
	uint64_t incarnation, seq;
	DeserializeParameters params{};
	if (!in.Read(id) || !in.Read(incarnation) || !in.Read(seq) || !seq || !in.Read(params.timeStamp))
		return;
	auto it = connection.incoming.find(id);
	if (it == connection.incoming.end() || it->second.incarnation != incarnation)
		return;
	for (unsigned i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; ++i)
		if (!in.Read(params.bitstreamWrittenTo[i]) || (params.bitstreamWrittenTo[i] && !ReadBlock(in, params.serializationBitstream[i])))
			return;
	if (in.GetNumberOfUnreadBits())
		return;
	bool changed = false;
	for (unsigned i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; ++i)
	{
		if (!params.bitstreamWrittenTo[i])
			continue;
		if (seq <= it->second.lastState[i])
		{
			params.bitstreamWrittenTo[i] = false;
			continue;
		}
		it->second.lastState[i] = seq;
		changed = true;
	}
	if (changed && delivery.Deserialize(id, &params, it->second.referenceIndex) && acknowledge)
		connection.Acknowledge(id, seq);
}
}
