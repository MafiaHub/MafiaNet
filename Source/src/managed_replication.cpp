// Copyright (c) 2026, MafiaHub. MIT license.
#include "managed_replication.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"
#include "mafianet/get_time.h"
#include <algorithm>
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
	    : Connection_RM3(session.address, ToGuid(guid)), runtime(runtime)
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
	uint64_t Watermark() const { return nextState; }
	bool Matches(uint64_t client, uint64_t server) const { return clientToken == client && serverToken == server; }
	ConstructionMode QueryConstructionMode() const override { return QUERY_CONNECTION_FOR_REPLICA_LIST; }
	bool QuerySerializationList(std::vector<Replica3 *> &out) override
	{
		// Retry this connection's previous pass before collecting more changes.
		// Explicit dirty marks accumulate until the connection can send again.
		if (!refusedStates.empty())
			return true;
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
		// Leave room for IP/UDP, encryption and reliability framing on this path.
		const int mtu = runtime.owner.PathMTU(systemAddress.systemIndex);
		batchLimit = static_cast<unsigned>(std::clamp(mtu - 96, 0, static_cast<int>(MaxBatchBytes)));
		const auto observer = ToPeerGuid(guid);
		std::vector<NetworkID> desired;
		runtime.interest.GetVisible(observer, desired);
		for (const auto &[id, object] : runtime.ordinary)
			if (!runtime.interest.Contains(id) && runtime.interest.Eligible(observer, id))
				desired.push_back(id);

		stats.desired = desired.size();
		std::set<NetworkID> wanted(desired.begin(), desired.end());
		std::vector<Replica3 *> constructed;
		GetConstructedReplicas(constructed);
		for (auto *object : constructed)
			if (!wanted.count(object->GetNetworkID()) || object->QueryDestruction(this, &runtime) == RM3DS_SEND_DESTRUCTION)
				leave.push_back(object);

		for (auto *object : leave)
			Forget(object->GetNetworkID());
		FlushRefusedStates();
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
			if (construction == RM3CS_SEND_CONSTRUCTION || construction == RM3CS_ALREADY_EXISTS_REMOTELY || construction == RM3CS_ALREADY_EXISTS_REMOTELY_DO_NOT_CONSTRUCT)
			{
				if (construction != RM3CS_SEND_CONSTRUCTION)
					existingArrivals.emplace(id, construction == RM3CS_ALREADY_EXISTS_REMOTELY);
				pending.try_emplace(id, tick);
				enter.push_back(object);
			}
		}
		const auto budget = runtime.interest.GetBudget(observer);
		if (budget && enter.size() > budget)
		{
			auto rank = [&](Replica3 *object)
			{
				const auto id = object->GetNetworkID();
				auto grant = runtime.grants.find(id);
				const bool required = grant != runtime.grants.end() && grant->second.holder == observer;
				return std::make_pair(!required, pending.at(id));
			};
			std::stable_sort(enter.begin(), enter.end(), [&](Replica3 *a, Replica3 *b)
			                 { return rank(a) < rank(b); });
			enter.resize(budget);
		}
		stats.constructed = constructed.size() - leave.size() + enter.size();
		stats.deferred = pending.size() - enter.size();
		for (auto *object : enter)
		{
			const auto id = object->GetNetworkID();
			pending.erase(id);
			if (runtime.explicitScheduling && runtime.types.at(static_cast<ReplicaTypeId>(object->registeredType)).detection == ChangeDetection::Compare)
				polled.insert(id);
		}
	}

	ReplicationStatistics stats;
	void ConstructionFailed(NetworkID id) override
	{
		if (auto *replica = runtime.owner.FindReplica(id))
			OnDereference(replica, &runtime);
		Forget(id);
		--stats.constructed;
		++stats.deferred;
	}
	void RecordScopeMessage(unsigned int bytes, bool entering, bool destroyed) override
	{
		stats.lifecycleBytes += bytes;
		if (entering)
			++stats.enters;
		else if (destroyed)
			++stats.destroys;
		else
			++stats.leaves;
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
		if (!refusedStates.empty())
			refusedStates.erase(std::remove_if(refusedStates.begin(), refusedStates.end(), [id](const PendingState &state)
			                                   { return state.id == id; }),
			                    refusedStates.end());
		dirty.erase(id);
		polled.erase(id);
		unconfirmed.erase(id);
		pending.erase(id);
	}
	void TransmitState(Replica3 &object, BitStream &out, PRO pro, RakPeerInterface &peer) override
	{
		const auto id = object.GetNetworkID();
		const Kind kind = object.replaceableSnapshot ? Kind::Snapshot : Kind::State;
		if (object.replaceableSnapshot)
		{
			auto &item = unconfirmed[id];
			item.seq = nextState;
			item.sent = GetTimeMS();
			item.bits = out.GetNumberOfBitsUsed();
			item.bytes.assign(out.GetData(), out.GetData() + out.GetNumberOfBytesUsed());
			pro = {pro.priority, Reliability::Unreliable, 0, 0};
		}
		// Receipt and sequenced modes have per-message semantics. Preserve those
		// boundaries, as well as immediate priority and oversized single records.
		const bool share = !pro.sendReceipt && pro.priority != Priority::Immediate &&
		                   (pro.reliability == Reliability::ReliableOrdered || pro.reliability == Reliability::Reliable || pro.reliability == Reliability::Unreliable);
		const auto bytes = out.GetNumberOfBytesUsed();
		if (!share || bytes <= HeaderBytes || bytes + sizeof(uint16_t) > batchLimit)
		{
			FlushBatch();
			SendSingle(id, kind, out, pro, peer);
			return;
		}
		const unsigned recordBytes = static_cast<unsigned>(bytes - HeaderBytes + sizeof(uint16_t));
		if (recordCount && (pro != batchPro || kind != batchKind || recordCount == MaxBatchRecords || batchBytes + recordBytes > batchLimit))
			FlushBatch();
		if (recordCount == 0)
		{
			first.Reset();
			first.WriteBits(out.GetData(), out.GetNumberOfBitsUsed(), false);
			firstId = id;
			batchKind = kind;
			batchPro = pro;
			batchBytes = HeaderBytes;
		}
		else
		{
			if (recordCount == 1)
			{
				batch.Reset();
				ReplicationWire::WriteHeader(batch, kind == Kind::Snapshot ? Kind::SnapshotBatch : Kind::StateBatch, clientToken, serverToken);
				AppendRecord(first);
			}
			AppendRecord(out);
		}
		batchBytes += recordBytes;
		++recordCount;
	}
	void FlushBatch()
	{
		if (recordCount == 0)
			return;
		auto &peer = *runtime.GetRakPeerInterface();
		if (recordCount == 1)
			SendSingle(firstId, batchKind, first, batchPro, peer);
		else if ((batchKind == Kind::Snapshot || refusedStates.empty()) && peer.Send(&batch, batchPro.priority, batchPro.reliability, batchPro.orderingChannel, guid, false))
			stats.stateBytes += batch.GetNumberOfBytesUsed();
		else if (batchKind == Kind::State)
		{
			// Keep retries addressable by object so a leave/destroy can cancel
			// its record without affecting the rest of a refused batch.
			batch.SetReadOffset(HeaderBytes * 8);
			StateBatchView view;
			if (view.Read(batch))
				for (unsigned i = 0; i < view.count; ++i)
				{
					const auto record = view.records[i];
					BitStream body(record.data, BITS_TO_BYTES(record.bits), false);
					NetworkID id;
					if (!body.Read(id))
						continue;
					BitStream single;
					ReplicationWire::WriteHeader(single, Kind::State, clientToken, serverToken);
					single.WriteBits(record.data, record.bits, false);
					QueueRefused(id, single, batchPro);
				}
		}
		recordCount = 0;
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
			{
				stats.stateBytes += retry.GetNumberOfBytesUsed();
				it = unconfirmed.erase(it);
			}
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
	BitStream first, batch;
	NetworkID firstId = UNASSIGNED_NETWORK_ID;
	Kind batchKind = Kind::State;
	PRO batchPro{};
	unsigned batchLimit = 0, batchBytes = 0, recordCount = 0;
	void AppendRecord(const BitStream &packet)
	{
		const auto bits = static_cast<uint16_t>(packet.GetNumberOfBitsUsed() - HeaderBytes * 8);
		batch.Write(bits);
		batch.WriteAlignedBytes(packet.GetData() + HeaderBytes, BITS_TO_BYTES(bits));
		if (const unsigned tail = bits % 8)
			batch.GetData()[batch.GetNumberOfBytesUsed() - 1] &= static_cast<unsigned char>(0xffu << (8 - tail));
	}
	void QueueRefused(NetworkID id, const BitStream &packet, PRO pro)
	{
		refusedStates.push_back({id, pro, packet.GetNumberOfBitsUsed(), {packet.GetData(), packet.GetData() + packet.GetNumberOfBytesUsed()}});
	}
	void SendSingle(NetworkID id, Kind kind, BitStream &packet, PRO pro, RakPeerInterface &peer)
	{
		if ((kind == Kind::Snapshot || refusedStates.empty()) && peer.Send(&packet, pro.priority, pro.reliability, pro.orderingChannel, guid, false, pro.sendReceipt))
			stats.stateBytes += packet.GetNumberOfBytesUsed();
		else if (kind == Kind::State)
			QueueRefused(id, packet, pro);
	}
	struct PendingState
	{
		NetworkID id;
		PRO pro;
		BitSize_t bits;
		std::vector<unsigned char> bytes;
	};
	std::deque<PendingState> refusedStates;
	void FlushRefusedStates()
	{
		while (!refusedStates.empty())
		{
			auto &state = refusedStates.front();
			BitStream retry(state.bytes.data(), static_cast<unsigned>(state.bytes.size()), false);
			retry.SetWriteOffset(state.bits);
			const auto &pro = state.pro;
			if (!runtime.GetRakPeerInterface()->Send(&retry, pro.priority, pro.reliability, pro.orderingChannel, guid, false, pro.sendReceipt))
				break;
			stats.stateBytes += state.bytes.size();
			refusedStates.pop_front();
		}
	}
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

ReplicationStatistics ManagedReplication::Statistics(PeerGuid guid)
{
	ReplicationStatistics sum;
	for (unsigned i = 0; i < GetConnectionCount(); ++i)
	{
		const auto target = GetConnectionAtIndex(i)->GetRakNetGUID();
		if (guid != UNASSIGNED_PEER_GUID && ToPeerGuid(target) != guid)
			continue;
		StateDelivery lock(*this, target);
		auto *c = static_cast<Connection *>(lock.GetConnection());
		if (!c)
			continue;
		const auto &s = c->stats;
		sum.desired += s.desired;
		sum.constructed += s.constructed;
		sum.deferred += s.deferred;
		sum.enters += s.enters;
		sum.leaves += s.leaves;
		sum.destroys += s.destroys;
		sum.lifecycleBytes += s.lifecycleBytes;
		sum.stateBytes += s.stateBytes;
	}
	return sum;
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
bool ManagedReplication::Send(PeerGuid guid, BitStream &packet)
{
	if (ticking)
	{
		StateDelivery delivery(*this, ToGuid(guid));
		if (auto *connection = static_cast<Connection *>(delivery.GetConnection()))
			connection->FlushBatch();
	}
	return GetRakPeerInterface()->Send(&packet, Priority::High, Reliability::ReliableOrdered, 0, ToGuid(guid), false);
}
void ManagedReplication::Connected(PeerGuid guid, const SystemAddress &address, bool incoming)
{
	if (owner.GetRole() != ReplicationRole::Peer && ((owner.GetRole() == ReplicationRole::Client) == incoming || (owner.GetRole() == ReplicationRole::Client && !sessions.empty())))
		return;
	auto inserted = sessions.try_emplace(guid);
	if (inserted.second)
	{
		inserted.first->second.address = address;
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
		Handshake(guid, session, Kind::Hello);
	}
	else if (session.helloReceived && session.status == ReplicationPeerStatus::Waiting)
		Handshake(guid, session, Kind::Accept);
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
void ManagedReplication::Handshake(PeerGuid guid, Session &session, Kind kind)
{
	if (kind == Kind::Accept && !session.serverToken)
	{
		randombytes_buf(&session.serverToken, sizeof(session.serverToken));
		if (!session.serverToken)
			session.serverToken = 1;
	}
	BitStream out;
	WriteHeader(out, kind, session);
	if (kind == Kind::Reject)
		out.Write(static_cast<uint8_t>(session.status));
	else
		out.Write(Schema());
	if (!Send(guid, out))
	{
		pendingHandshakes[guid] = kind;
		return;
	}
	if (kind == Kind::Accept && Activate(guid, session) && connectionChanged)
		connectionChanged(guid, true);
}

bool ManagedReplication::SendRevoke(const Revocation &revocation)
{
	BitStream out;
	WriteHeader(out, Kind::Revoke, sessions.at(revocation.holder));
	out.Write(revocation.id);
	out.Write(revocation.epoch);
	return Send(revocation.holder, out);
}

void ManagedReplication::RetryControl()
{
	auto handshakes = std::move(pendingHandshakes);
	pendingHandshakes.clear();
	for (auto [guid, kind] : handshakes)
	{
		auto session = sessions.find(guid);
		if (session != sessions.end() && !closing.count(guid) && !resetRequested)
			Handshake(guid, session->second, kind);
	}
	pendingRevocations.erase(std::remove_if(pendingRevocations.begin(), pendingRevocations.end(),
	                                        [this](const Revocation &r)
	                                        { return SendRevoke(r); }),
	                         pendingRevocations.end());
}

void ManagedReplication::Notify(ReplicaEventKind kind, NetworkID id, PeerGuid source)
{
	if (replicaChanged)
		events.push_back({kind, id, source});
}
void ManagedReplication::DispatchEvents()
{
	if (dispatching || events.empty())
		return;
	dispatching = true;
	struct DispatchScope
	{
		bool &active;
		~DispatchScope() { active = false; }
	} scope{dispatching};
	while (!events.empty())
	{
		auto event = events.front();
		events.pop_front();
		auto callback = replicaChanged;
		if (callback)
			(*callback)(event);
	}
}
bool ManagedReplication::Release(NetworkID id, ReplicaEventKind reason)
{
	auto it = downloads.find(id);
	if (it == downloads.end())
		return false;
	const auto type = it->second.type;
	const auto source = it->second.source;
	if (grants.erase(id))
		Notify(ReplicaEventKind::AuthorityLost, id, source);
	Notify(reason, id, source);
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
	if (HoldsConnectionLock())
		return;
	WorldGuard lock(*this);
	if (ticking)
		return;
	ticking = true;
	TickScope scope{*this};
	if (beforeTick)
		beforeTick();
	if (resetRequested)
		return;
	RetryControl();
	if (resetRequested)
		return;
	UpdateGrants(false);
	Update();
	if (resetRequested)
		return;
	// Update has joined every serialization worker. Only the outgoing pass
	// touches these buffers, and the world guard keeps each connection alive.
	for (unsigned i = 0; i < GetConnectionCount(); ++i)
		static_cast<Connection *>(GetConnectionAtIndex(i))->FlushBatch();
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
	pendingHandshakes.erase(guid);
	pendingRevocations.erase(std::remove_if(pendingRevocations.begin(), pendingRevocations.end(),
	                                        [guid](const Revocation &r)
	                                        { return r.holder == guid; }),
	                         pendingRevocations.end());
	sessions.erase(it);
	interest.RemoveObserver(guid);
	for (auto grant = grants.begin(); grant != grants.end();)
	{
		if (grant->second.holder == guid && !IsDownload(grant->first))
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
		Release(id, ReplicaEventKind::Disconnected);
	{
		StateDelivery delivery(*this, ToGuid(guid));
		if (auto *conn = static_cast<Connection *>(delivery.GetConnection()))
			for (const auto &visit : conn->incoming)
				Notify(ReplicaEventKind::Disconnected, visit.first, guid);
	}
	delete PopConnection(ToGuid(guid));
	DispatchEvents();
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
		{
			connection->FlushBatch();
			connection->Forget(id);
			connection->incoming.erase(id);
		}
	}
	downloads.erase(id);
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
		{
			connection->FlushBatch();
			connection->SendRemoval(replica, true, *GetRakPeerInterface());
		}
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
	Header header{};
	if (!ReadHeader(in, header) || !header.clientToken)
		return;
	const auto &[version, kind, clientToken, serverToken] = header;

	const bool batched = kind == Kind::StateBatch || kind == Kind::SnapshotBatch;
	if (batched || kind == Kind::State || kind == Kind::Snapshot || kind == Kind::SnapshotAck)
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
		else if (batched)
		{
			StateBatchView view;
			if (!view.Read(in))
				return;
			for (unsigned i = 0; i < view.count && delivery.GetConnection(); ++i)
			{
				const auto record = view.records[i];
				BitStream body(record.data, BITS_TO_BYTES(record.bits), false);
				body.SetWriteOffset(record.bits);
				ReadState(*connection, delivery, body, kind == Kind::SnapshotBatch);
			}
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
			Handshake(guid, session, Kind::Reject);
		}
		else if (session.admitted)
			Handshake(guid, session, Kind::Accept);
		return;
	}
	const bool waitingForReject = !server && session.status == ReplicationPeerStatus::Waiting && kind == Kind::Reject && !serverToken;
	if ((version != Version && !waitingForReject) || clientToken != session.clientToken || !session.admitted)
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
			    ((version == Version && reason == static_cast<uint8_t>(ReplicationPeerStatus::SchemaMismatch)) || reason == static_cast<uint8_t>(ReplicationPeerStatus::ProtocolMismatch)))
				session.status = static_cast<ReplicationPeerStatus>(reason);
		}
		return;
	}
	if (session.status != ReplicationPeerStatus::Active || session.serverToken != serverToken)
		return;
	{
		StateDelivery delivery(*this, packet.guid);
		auto *conn = static_cast<Connection *>(delivery.GetConnection());
		if (!conn)
			return;
		if (kind == Kind::OwnerUpdate || kind == Kind::Grant || kind == Kind::Revoke)
			OwnerPacket(*conn, kind, in);
		else if (owner.GetRole() == ReplicationRole::Peer || !server)
			Apply(*conn, kind, in);
	}
	DispatchEvents();
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
	if (guid != UNASSIGNED_PEER_GUID && (nextEpoch == UINT64_MAX || object.staticBinding || Status(guid) != ReplicationPeerStatus::Active || !interest.Eligible(guid, id)))
		return false;
	uint64_t publishedEpoch = 0;
	auto current = grants.find(id);
	if (current != grants.end())
	{
		if (current->second.holder == guid && !force)
			return true;
		if (current->second.holder == guid)
			publishedEpoch = current->second.publishedEpoch;
		else if (current->second.publishedEpoch)
		{
			const Revocation revoke{current->second.holder, id, current->second.publishedEpoch};
			if (!SendRevoke(revoke))
				pendingRevocations.push_back(revoke);
		}
		grants.erase(current);
	}
	else if (guid == UNASSIGNED_PEER_GUID)
		return false;
	interest.Require(id, guid);
	if (guid != UNASSIGNED_PEER_GUID)
		grants.emplace(id, Grant{guid, ++nextEpoch, 0, publishedEpoch});
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
	if (grant.sequence == UINT64_MAX)
		return false;
	out.Write(grant.sequence + 1);
	if (!WriteBlock(out, payload) || !Send(grant.holder, out))
		return false;
	++grant.sequence;
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
		auto *connection = static_cast<Connection *>(GetConnectionByGUID(ToGuid(grant.holder)));
		if (!object || !connection)
			continue;
		if (!interest.Eligible(grant.holder, id) || (grant.publishedEpoch && !connection->HasReplicaConstructed(object)))
		{
			Assign(*object, UNASSIGNED_PEER_GUID);
			continue;
		}
		if (!publish || grant.Ready() || !connection->HasReplicaConstructed(object))
			continue;
		BitStream snapshot, out;
		object->SerializeConstruction(&snapshot, connection);
		it = grants.find(id);
		if (it == grants.end() || it->second.epoch != grant.epoch)
			continue;
		WriteHeader(out, Kind::Grant, sessions.at(grant.holder));
		out.Write(id);
		out.Write(grant.epoch);
		out.Write(connection->Watermark());
		if (!WriteBlock(out, snapshot))
			continue;
		if (Send(grant.holder, out))
			it->second.publishedEpoch = grant.epoch;
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
		if (owner.GetRole() != ReplicationRole::Server || it == grants.end() || it->second.holder != sender || it->second.epoch != epoch || !it->second.Ready() || !interest.Eligible(sender, id) || !connection.HasReplicaConstructed(object))
			return;
		uint64_t seq;
		BitStream payload;
		if (!in.Read(seq) || !seq || seq <= it->second.sequence || !ReadBlock(in, payload) || in.GetNumberOfUnreadBits())
			return;
		it->second.sequence = seq;
		if (reports.at(static_cast<ReplicaTypeId>(object->registeredType)).accept(*object, sender, payload) && owner.FindReplica(id) == object)
			Mark(*object);
		return;
	}
	if (owner.GetRole() != ReplicationRole::Client || !downloads.count(id) || downloads.at(id).source != sender)
		return;
	if (kind == Kind::Revoke)
	{
		if (!in.GetNumberOfUnreadBits() && it != grants.end() && it->second.epoch == epoch)
		{
			grants.erase(it);
			Notify(ReplicaEventKind::AuthorityLost, id, sender);
		}
		return;
	}
	const bool correction = it != grants.end();
	if (correction && epoch <= it->second.epoch)
		return;
	uint64_t watermark;
	BitStream snapshot;
	if (!in.Read(watermark) || !ReadBlock(in, snapshot) || in.GetNumberOfUnreadBits() || !reports.count(static_cast<ReplicaTypeId>(object->registeredType)))
		return;
	const auto before = generation;
	const auto currentVisit = connection.incoming.find(id);
	if (currentVisit == connection.incoming.end())
		return;
	// A complete snapshot on the unreliable channel can overtake this reliable grant.
	const bool newerSnapshot = types.at(static_cast<ReplicaTypeId>(object->registeredType)).delivery == SnapshotDelivery::Replaceable &&
	                           currentVisit->second.lastState[0] > watermark;
	if ((newerSnapshot || object->DeserializeConstruction(&snapshot, &connection)) && generation == before && owner.FindReplica(id) == object)
	{
		auto visit = connection.incoming.find(id);
		if (visit == connection.incoming.end())
			return;
		for (auto &seq : visit->second.lastState)
			seq = std::max(seq, watermark);
		grants.insert_or_assign(id, Grant{sender, epoch, 0, epoch});
		Notify(correction ? ReplicaEventKind::AuthorityCorrected : ReplicaEventKind::AuthorityGranted, id, sender);
	}
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
		const auto reason = kind == Kind::Destroy ? ReplicaEventKind::Destroyed : ReplicaEventKind::LeftScope;
		if (!Release(id, reason))
			Notify(reason, id, ToPeerGuid(connection.GetRakNetGUID()));
		return;
	}
	auto *replica = static_cast<Replica3 *>(GetNetworkIDManager()->GET_BASE_OBJECT_FROM_ID(id));
	if ((kind == Kind::Spawn && !replica) || (kind == Kind::Bind && replica && staticObjects.count(id)))
	{
		ReplicaTypeId type;
		BitStream snapshot;
		bool baseline = true;
		if (!in.Read(type) || (kind == Kind::Bind && !in.Read(baseline)) || !ReadBlock(in, snapshot) || in.GetNumberOfUnreadBits() || (!baseline && snapshot.GetNumberOfBitsUsed()))
			return;
		auto entry = types.find(type);
		if (entry == types.end())
			return;
		if (kind == Kind::Bind)
		{
			const auto source = ToPeerGuid(connection.GetRakNetGUID());
			const auto before = generation;
			if (!connection.incoming.count(id) && entry->second.cppType == typeid(*replica) && replica->QueryRemoteConstruction(&connection) &&
			    before == generation && owner.FindReplica(id) == replica)
			{
				connection.incoming.insert_or_assign(id, Connection::Visit{visit, replica->referenceIndex});
				if (baseline)
					replica->DeserializeConstructionExisting(&snapshot, &connection);
				if (before == generation && owner.FindReplica(id) == replica && connection.incoming.count(id))
					Notify(ReplicaEventKind::EnteredScope, id, source);
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
		const auto source = ToPeerGuid(connection.GetRakNetGUID());
		downloads.emplace(id, Download{type, source});
		Notify(ReplicaEventKind::EnteredScope, id, source);
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
	if (changed && delivery.Deserialize(id, &params, it->second.referenceIndex) && acknowledge && delivery.GetConnection())
		connection.Acknowledge(id, seq);
}
}
