// Copyright (c) 2026, MafiaHub. MIT license.

#include "mafianet/replication_manager.h"
#include "managed_replication.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/replica_manager3.h"

namespace MafiaNet
{
ReplicationManager::ReplicationManager(RakPeerInterface &peer)
    : peer(peer)
{
}

ReplicationManager::~ReplicationManager() = default;

bool ReplicationManager::SetRole(ReplicationRole requested)
{
	if (requested == role)
		return true;
	if (managed && (managed->GetReplicaCount() || managed->HasSessions()))
		return false;
	role = requested;
	if (requested != ReplicationRole::Unconfigured)
		Managed();
	return true;
}

bool ReplicationManager::Reference(Replica3 *replica)
{
	if ((role != ReplicationRole::Server && role != ReplicationRole::Peer) || !replica || replica->WasReferenced())
		return false;
	if (!managed || !managed->SetRegisteredType(*replica))
		return false;
	WorldGuard guard(*this);
	replica->staticBinding = false;
	managed->Reference(replica);
	managed->ordinary.emplace(replica->GetNetworkID(), replica);
	return true;
}

InterestGrid &ReplicationManager::GetInterestGrid()
{
	return Managed().interest;
}

bool ReplicationManager::Reference(Replica3 *replica, SpatialScope scope)
{
	WorldGuard guard(*this);
	if (!Reference(replica))
		return false;
	const auto id = replica->GetNetworkID();
	if (!managed->interest.Track(id, scope))
	{
		managed->Dereference(replica);
		return false;
	}
	managed->ordinary.erase(id);
	return true;
}

bool ReplicationManager::ReferenceStatic(Replica3 *replica, NetworkID id)
{
	if (!replica || id == UNASSIGNED_NETWORK_ID || FindReplica(id) || replica->WasReferenced() || !managed || !managed->SetRegisteredType(*replica))
		return false;
	replica->staticBinding = true;
	replica->SetNetworkID(id);
	managed->Reference(replica);
	managed->staticObjects[id] = replica;
	managed->ordinary.emplace(id, replica);
	return true;
}
ManagedReplication &ReplicationManager::Managed()
{
	if (!managed)
	{
		ids = std::make_unique<NetworkIDManager>();
		managed = std::make_unique<ManagedReplication>(*this, peer);
		managed->SetNetworkIDManager(ids.get());
	}
	return *managed;
}

bool ReplicationManager::RegisterType(ReplicaTypeId id, uint16_t version, std::type_index type, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release)
{
	return Managed().Register(id, version, type, std::move(create), std::move(release));
}

Replica3 *ReplicationManager::FindReplica(NetworkID id) const
{
	return ids ? static_cast<Replica3 *>(ids->GET_BASE_OBJECT_FROM_ID(id)) : nullptr;
}

bool ReplicationManager::Destroy(Replica3 *replica)
{
	if (!managed || role == ReplicationRole::Client || !replica || replica->replicaManager != managed.get())
		return false;
	WorldGuard guard(*this);
	managed->Destroy(*replica);
	return true;
}

ReplicationManager::WorldGuard::WorldGuard(ReplicationManager &manager)
    : lock(manager.Managed().worldMutex) {}
bool ReplicationManager::ReleaseRemote(NetworkID id)
{
	if (!managed)
		return false;
	WorldGuard guard(*this);
	return managed->Release(id);
}
ReplicationPeerStatus ReplicationManager::GetPeerStatus(PeerGuid guid) const
{
	return managed ? managed->Status(guid) : ReplicationPeerStatus::Waiting;
}

bool ReplicationManager::Admit(PeerGuid guid)
{
	return managed && managed->Admit(guid);
}
void ReplicationManager::SetAutomaticAdmission(bool enabled)
{
	Managed().automaticAdmission = enabled;
}

void ReplicationManager::Dereference(Replica3 *replica)
{
	if (managed && role != ReplicationRole::Client && replica && replica->replicaManager == managed.get())
	{
		managed->staticObjects.erase(replica->GetNetworkID());
		managed->Dereference(replica);
	}
}

unsigned int ReplicationManager::GetReplicaCount() const
{
	return managed ? managed->GetReplicaCount() : 0;
}

unsigned int ReplicationManager::GetConnectionCount() const
{
	return managed ? managed->GetConnectionCount() : 0;
}
Replica3 *ReplicationManager::GetReplicaAtIndex(unsigned int index) const
{
	return managed && index < managed->GetReplicaCount() ? managed->GetReplicaAtIndex(index) : nullptr;
}
void ReplicationManager::GetLocalReplicas(std::vector<Replica3 *> &out) const
{
	out.clear();
	if (managed)
		managed->GetReplicasCreatedByMe(out);
}
bool ReplicationManager::SetObserverVirtualWorld(PeerGuid guid, uint32_t dimension)
{
	auto *connection = managed ? managed->GetConnectionByGUID(ToGuid(guid)) : nullptr;
	if (!connection)
		return false;
	connection->SetVirtualWorld(dimension);
	return true;
}
void ReplicationManager::SetSerializeInterval(Time interval)
{
	Managed().SetAutoSerializeInterval(interval);
}
void ReplicationManager::SetSerializeExecutor(RM3SerializeExecutor *executor)
{
	Managed().SetSerializeExecutor(executor);
}
void ReplicationManager::SetSerializationList(std::function<bool(PeerGuid, std::vector<Replica3 *> &)> query)
{
	Managed().serializationList = std::move(query);
}
void ReplicationManager::SetTickCallbacks(std::function<void()> before, std::function<void()> after)
{
	Managed().beforeTick = std::move(before);
	Managed().afterTick = std::move(after);
}
void ReplicationManager::SetConnectionCallback(std::function<void(PeerGuid, bool)> callback)
{
	Managed().connectionChanged = std::move(callback);
}
void ReplicationManager::Tick()
{
	if (managed)
		managed->Tick();
}

void ReplicationManager::ReceiveTick()
{
	if (tickMode == ReplicationTickMode::Automatic)
		Tick();
}

bool ReplicationManager::ProcessPacket(Packet *packet)
{
	if (!packet->length)
		return false;
	if (packet->data[0] == ID_REPLICATION)
	{
		if (managed && role != ReplicationRole::Unconfigured)
		{
			WorldGuard guard(*this);
			managed->Receive(*packet);
		}
		return true;
	}
	if (!managed || role == ReplicationRole::Unconfigured)
		return false;
	WorldGuard guard(*this);
	switch (packet->data[0])
	{
	case ID_CONNECTION_REQUEST_ACCEPTED:
	case ID_NEW_INCOMING_CONNECTION:
		managed->Connected(ToPeerGuid(packet->guid), packet->data[0] == ID_NEW_INCOMING_CONNECTION);
		break;
	case ID_CONNECTION_LOST:
	case ID_DISCONNECTION_NOTIFICATION:
		managed->Disconnected(ToPeerGuid(packet->guid));
		break;
	}
	return false;
}

void ReplicationManager::Shutdown()
{
	if (managed)
		managed->Reset();
}
}
