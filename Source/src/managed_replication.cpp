// Copyright (c) 2026, MafiaHub. MIT license.
#include "managed_replication.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"
#include <limits>
#include <sodium.h>

namespace MafiaNet
{
namespace
{
constexpr uint8_t protocolVersion = 1;
constexpr BitSize_t maxPayloadBits = 8 * 1024 * 1024;
bool WriteBlock(BitStream &out, BitStream &data)
{
	const auto bits = data.GetNumberOfBitsUsed();
	if (bits > maxPayloadBits)
		return false;
	out.Write(bits);
	if (bits)
		out.WriteBits(data.GetData(), bits, false);
	return true;
}
bool ReadBlock(BitStream &in, BitStream &data)
{
	BitSize_t bits;
	return in.Read(bits) && bits <= maxPayloadBits && bits <= in.GetNumberOfUnreadBits() && (!bits || in.Read(data, bits));
}
}

class ManagedReplication::Connection final : public Connection_RM3
{
public:
	Connection(ManagedReplication &runtime, PeerGuid guid, const Session &session)
	    : Connection_RM3(runtime.GetRakPeerInterface()->GetSystemAddressFromGuid(ToGuid(guid)), ToGuid(guid)), runtime(runtime), session(session)
	{
		isValidated = true;
	}
	Replica3 *AllocReplica(BitStream *, ReplicaManager3 *) override { return nullptr; }
	ConstructionMode QueryConstructionMode() const override { return QUERY_CONNECTION_FOR_REPLICA_LIST; }
	bool QuerySerializationList(std::vector<Replica3 *> &out) override
	{
		return runtime.serializationList && runtime.serializationList(ToPeerGuid(guid), out);
	}
	void QueryReplicaList(std::vector<Replica3 *> &enter, std::vector<Replica3 *> &leave) override
	{
		if (runtime.owner.GetRole() == ReplicationRole::Client)
			return;
		for (unsigned int i = 0; i < runtime.GetReplicaCount(); ++i)
		{
			auto *replica = runtime.GetReplicaAtIndex(i);
			if (replica->creatingSystemGUID != runtime.GetRakPeerInterface()->GetMyGUID())
				continue;
			if (!HasReplicaConstructed(replica))
			{
				const auto construction = replica->QueryConstruction(this, &runtime);
				if (construction == RM3CS_SEND_CONSTRUCTION || construction == RM3CS_ALREADY_EXISTS_REMOTELY)
					enter.push_back(replica);
			}
			else if (replica->QueryDestruction(this, &runtime) == RM3DS_SEND_DESTRUCTION)
				leave.push_back(replica);
		}
	}
	void SendConstruction(std::vector<Replica3 *> &enter, std::vector<Replica3 *> &leave, PRO, RakPeerInterface *, unsigned char, ReplicaManager3 *) override
	{
		for (auto *replica : enter)
		{
			BitStream out, snapshot;
			const bool bind = runtime.staticObjects.count(replica->GetNetworkID()) != 0;
			runtime.WriteHeader(out, bind ? Kind::Bind : Kind::Spawn, session);
			out.Write(replica->GetNetworkID());
			out.Write(runtime.typeIds.at(typeid(*replica)));
			if (bind)
				replica->SerializeConstructionExisting(&snapshot, this);
			else
				replica->SerializeConstruction(&snapshot, this);
			if (WriteBlock(out, snapshot))
				runtime.Send(ToPeerGuid(guid), out);
		}
		for (auto *replica : leave)
			Remove(*replica, Kind::Leave);
	}
	void Remove(Replica3 &replica, Kind kind)
	{
		BitStream out;
		runtime.WriteHeader(out, kind, session);
		out.Write(replica.GetNetworkID());
		runtime.Send(ToPeerGuid(guid), out);
	}
	SendSerializeIfChangedResult SendSerialize(Replica3 *replica, bool selected[], BitStream channels[], Time timestamp, PRO pro[], RakPeerInterface *, unsigned char, Time) override
	{
		BitStream out;
		runtime.WriteHeader(out, Kind::State, session);
		out.Write(replica->GetNetworkID());
		out.Write(timestamp);
		for (unsigned int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; ++i)
		{
			out.Write(selected[i]);
			if (selected[i] && !WriteBlock(out, channels[i]))
				return SSICR_DID_NOT_SEND_DATA;
		}
		for (unsigned int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; ++i)
			if (selected[i])
			{
				runtime.GetRakPeerInterface()->Send(&out, pro[i].priority, pro[i].reliability, pro[i].orderingChannel, guid, false, pro[i].sendReceipt);
				break;
			}
		return SSICR_SENT_DATA;
	}

private:
	ManagedReplication &runtime;
	const Session session;
};

bool ManagedReplication::Register(ReplicaTypeId id, uint16_t version, std::type_index type, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release)
{
	if (!sessions.empty() || !create || !release || types.count(id) || typeIds.count(type))
		return false;
	types.emplace(id, Type{type, version, std::move(create), std::move(release)});
	typeIds.emplace(type, id);
	return true;
}

bool ManagedReplication::IsRegistered(const Replica3 &replica) const
{
	return typeIds.count(typeid(replica)) != 0;
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
	hash ^= owner.GetRole() == ReplicationRole::Peer ? 1 : 0;
	return hash;
}
void ManagedReplication::WriteHeader(BitStream &out, Kind kind, const Session &session) const
{
	out.Write(static_cast<MessageID>(ID_REPLICATION));
	out.Write(protocolVersion);
	out.Write(kind);
	out.Write(session.clientToken);
	out.Write(session.serverToken);
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
	if (beforeTick)
		beforeTick();
	Update();
	if (afterTick)
		afterTick();
	ticking = false;
}
void ManagedReplication::Disconnected(PeerGuid guid)
{
	auto it = sessions.find(guid);
	if (it == sessions.end())
		return;
	const bool active = it->second.status == ReplicationPeerStatus::Active;
	sessions.erase(it);
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
void ManagedReplication::Reset()
{
	++generation;
	while (!sessions.empty())
		Disconnected(sessions.begin()->first);
	staticObjects.clear();
	OnRakPeerShutdown();
}
void ManagedReplication::Destroy(Replica3 &replica)
{
	for (unsigned int i = 0; i < GetConnectionCount(); ++i)
	{
		auto *connection = static_cast<Connection *>(GetConnectionAtIndex(i));
		if (connection->HasReplicaConstructed(&replica))
			connection->Remove(replica, Kind::Destroy);
	}
	Dereference(&replica);
}

void ManagedReplication::Receive(Packet &packet)
{
	const auto guid = ToPeerGuid(packet.guid);
	auto it = sessions.find(guid);
	if (it == sessions.end() || packet.length > (maxPayloadBits / 8) * RM3_NUM_OUTPUT_BITSTREAM_CHANNELS + 256 ||
	    packet.bitSize > static_cast<uint64_t>(packet.length) * 8)
		return;
	BitStream in(packet.data, packet.length, false);
	in.SetWriteOffset(packet.bitSize);
	in.IgnoreBytes(1);
	uint8_t version;
	Kind kind;
	uint64_t clientToken, serverToken;
	if (!in.Read(version) || !in.Read(kind) || !in.Read(clientToken) || !in.Read(serverToken) || !clientToken)
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
		if (version != protocolVersion || schema != Schema())
		{
			session.status = version != protocolVersion ? ReplicationPeerStatus::ProtocolMismatch : ReplicationPeerStatus::SchemaMismatch;
			BitStream out;
			WriteHeader(out, Kind::Reject, session);
			out.Write(static_cast<uint8_t>(session.status));
			Send(guid, out);
		}
		else if (session.admitted)
			Start(guid, session);
		return;
	}
	if (version != protocolVersion || clientToken != session.clientToken || !session.admitted)
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
	if ((owner.GetRole() == ReplicationRole::Peer || !server) && session.status == ReplicationPeerStatus::Active && session.serverToken == serverToken)
		if (auto *conn = static_cast<Connection *>(GetConnectionByGUID(packet.guid)))
			Apply(*conn, kind, in);
}

void ManagedReplication::Apply(Connection &connection, Kind kind, BitStream &in)
{
	NetworkID id;
	if (!in.Read(id) || id == UNASSIGNED_NETWORK_ID)
		return;
	if (kind == Kind::Leave || kind == Kind::Destroy)
	{
		auto it = downloads.find(id);
		if (!in.GetNumberOfUnreadBits() && it != downloads.end() && it->second.source == ToPeerGuid(connection.GetRakNetGUID()))
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
				replica->DeserializeConstructionExisting(&snapshot, &connection);
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
		copy->creatingSystemGUID = connection.GetRakNetGUID();
		copy->SetNetworkID(id);
		Reference(copy);
		downloads.emplace(id, Download{type, ToPeerGuid(connection.GetRakNetGUID())});
	}
	else if (kind == Kind::State && replica && (replica->creatingSystemGUID == connection.GetRakNetGUID() || (staticObjects.count(id) && replica->QueryRemoteConstruction(&connection))))
	{
		DeserializeParameters params{};
		params.sourceConnection = &connection;
		if (!in.Read(params.timeStamp))
			return;
		for (unsigned int i = 0; i < RM3_NUM_OUTPUT_BITSTREAM_CHANNELS; ++i)
			if (!in.Read(params.bitstreamWrittenTo[i]) || (params.bitstreamWrittenTo[i] && !ReadBlock(in, params.serializationBitstream[i])))
				return;
		if (!in.GetNumberOfUnreadBits())
			replica->Deserialize(&params);
	}
}
}
