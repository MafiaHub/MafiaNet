// Copyright (c) 2026, MafiaHub. MIT license.

#include "mafianet/replication_manager.h"
#include "managed_replication.h"
#include "mafianet/peer.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/replica_manager3.h"

namespace MafiaNet
{
ReplicationManager::ReplicationManager(RakPeer &peer)
    : peer(peer)
{
}

ReplicationManager::~ReplicationManager() = default;

unsigned ReplicationManager::BatchPayloadLimit(SystemIndex index) const
{
	// The connection event captured this slot. Its MTU is only a sizing hint;
	// the GUID remains the destination even when the transport recycles a slot.
	const int mtu = index < peer.maximumNumberOfPeers && peer.remoteSystemList ? peer.remoteSystemList[index].MTUSize.load(std::memory_order_relaxed) : 0;
	// ReliableOrdered has the largest unsplit header among the batchable modes.
	return ReliabilityLayer::MaxUnfragmentedPayload(mtu, peer.GetEncryptionMode(), Reliability::ReliableOrdered);
}

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
	WorldGuard guard(*this);
	if ((role != ReplicationRole::Server && role != ReplicationRole::Peer) || !replica || replica->WasReferenced())
		return false;
	if (!managed || (replica->GetNetworkID() != UNASSIGNED_NETWORK_ID && FindReplica(replica->GetNetworkID())) || !managed->SetRegisteredType(*replica))
		return false;
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
	WorldGuard guard(*this);
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

bool ReplicationManager::RegisterType(ReplicaTypeId id, uint16_t version, std::type_index type, bool snapshot, bool timed, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release)
{
	return Managed().Register(id, version, type, snapshot, timed, std::move(create), std::move(release));
}

Replica3 *ReplicationManager::FindReplica(NetworkID id) const
{
	return ids ? static_cast<Replica3 *>(ids->GET_BASE_OBJECT_FROM_ID(id)) : nullptr;
}

bool ReplicationManager::Destroy(Replica3 *replica)
{
	WorldGuard guard(*this);
	if (role == ReplicationRole::Client || !replica || replica->replicaManager != managed.get() || managed->IsDownload(replica->GetNetworkID()))
		return false;
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
	const bool released = managed->Release(id);
	managed->DispatchEvents();
	return released;
}
ReplicationStatistics ReplicationManager::GetStatistics(PeerGuid guid) const
{
	if (!managed)
		return {};
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->Statistics(guid);
}
ReplicationPeerStatus ReplicationManager::GetPeerStatus(PeerGuid guid) const
{
	return managed ? managed->Status(guid) : ReplicationPeerStatus::Waiting;
}

bool ReplicationManager::RegisterReport(std::type_index object, std::type_index type, uint16_t version, std::function<bool(Replica3 &, PeerGuid, BitStream &)> accept)
{
	WorldGuard guard(*this);
	return Managed().RegisterReport(object, {type, version, std::move(accept)});
}
bool ReplicationManager::SetOwnerUpdateRateLimit(ReplicaTypeId id, uint32_t reportsPerSecond, uint32_t burst)
{
	WorldGuard guard(*this);
	return managed->LimitReports(id, reportsPerSecond, burst);
}
bool ReplicationManager::SetSnapshotPolicy(ReplicaTypeId id, ChangeDetection detection, SnapshotDelivery delivery)
{
	WorldGuard guard(*this);
	return managed->Policy(id, detection, delivery);
}
bool ReplicationManager::MarkDirty(Replica3 &object)
{
	WorldGuard guard(*this);
	return managed->Mark(object);
}
void ReplicationManager::MarkAllDirty()
{
	WorldGuard guard(*this);
	managed->MarkAll();
}
bool ReplicationManager::SetAuthority(Replica3 &object, PeerGuid guid)
{
	WorldGuard guard(*this);
	return guid != UNASSIGNED_PEER_GUID && managed->Assign(object, guid);
}
bool ReplicationManager::RevokeAuthority(Replica3 &object)
{
	WorldGuard guard(*this);
	return managed->Assign(object, UNASSIGNED_PEER_GUID);
}
bool ReplicationManager::CorrectAuthority(Replica3 &object)
{
	WorldGuard guard(*this);
	return managed->Correct(object);
}
bool ReplicationManager::HasLocalAuthority(Replica3 &object) const
{
	if (!managed)
		return false;
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->Owns(object);
}
OwnerInputStamp ReplicationManager::GetLocalInputStamp(Replica3 &object) const
{
	if (!managed)
		return {};
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->LocalStamp(object);
}
bool ReplicationManager::SendReport(Replica3 &object, std::type_index type, const BitStream &payload, OwnerInputStamp *sent)
{
	WorldGuard guard(*this);
	return managed->Submit(object, type, payload, sent);
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
	WorldGuard guard(*this);
	if (replica && replica->replicaManager == managed.get() && !managed->IsDownload(replica->GetNetworkID()))
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
	managed->interest.SetObserverDimension(guid, dimension);
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
void ReplicationManager::SetReplicaCallback(std::function<void(const ReplicaEvent &)> callback)
{
	WorldGuard guard(*this);
	managed->replicaChanged = callback ? std::make_shared<ManagedReplication::ReplicaCallback>(std::move(callback)) : nullptr;
}
void ReplicationManager::SetConnectionCallback(std::function<void(PeerGuid, bool)> callback)
{
	Managed().connectionChanged = std::move(callback);
}
void ReplicationManager::Tick()
{
	if (managed)
	{
		managed->Tick();
		DeliverDelegation();
	}
}

bool ReplicationManager::Tick(Time simulationTick)
{
	if (!managed || !simulationTick)
		return false;
	const bool ticked = managed->Tick(simulationTick);
	DeliverDelegation();
	return ticked;
}
void ReplicationManager::DeliverDelegation()
{
	if (managed && role == ReplicationRole::Server)
		managed->delegation.Deliver(managed->worldMutex);
}
bool ReplicationManager::SetDelegation(Replica3 &replica, const DelegationPolicy *policy)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.Set(replica, policy);
}
void ReplicationManager::SetDelegationInterval(uint32_t milliseconds)
{
	if (role != ReplicationRole::Server)
		return;
	WorldGuard guard(*this);
	managed->delegation.SetInterval(milliseconds);
}
void ReplicationManager::SetClientCapacity(uint32_t maxWeight)
{
	if (role != ReplicationRole::Server)
		return;
	WorldGuard guard(*this);
	managed->delegation.SetDefaultCapacity(maxWeight);
}
bool ReplicationManager::SetClientCapacity(PeerGuid guid, uint32_t maxWeight)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.SetCapacity(guid, maxWeight);
}
bool ReplicationManager::Suspend(PeerGuid guid)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.Suspend(guid);
}
bool ReplicationManager::Resume(PeerGuid guid)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.Resume(guid);
}
bool ReplicationManager::IsSuspended(PeerGuid guid) const
{
	if (role != ReplicationRole::Server || !managed)
		return false;
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->delegation.IsSuspended(guid);
}
bool ReplicationManager::Pin(Replica3 &replica, PeerGuid guid)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.Pin(replica, guid);
}
bool ReplicationManager::Unpin(Replica3 &replica)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.Unpin(replica);
}
bool ReplicationManager::IsPinned(Replica3 &replica) const
{
	if (role != ReplicationRole::Server || !managed)
		return false;
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->delegation.IsPinned(replica);
}
void ReplicationManager::SetDelegationCallback(std::function<void(NetworkID, PeerGuid, PeerGuid, DelegationChange)> callback)
{
	if (role != ReplicationRole::Server)
		return;
	WorldGuard guard(*this);
	managed->delegation.SetCallback(std::move(callback));
}
DelegationStatistics ReplicationManager::GetDelegationStatistics() const
{
	if (role != ReplicationRole::Server || !managed)
		return {};
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->delegation.Statistics();
}
uint32_t ReplicationManager::GetDelegatedLoad(PeerGuid guid) const
{
	if (role != ReplicationRole::Server || !managed)
		return 0;
	ReplicaManager3::WorldGuard guard(*managed);
	return managed->delegation.Load(guid);
}
void ReplicationManager::SetReleaseVeto(std::function<bool(NetworkID, PeerGuid)> canRelease)
{
	if (role != ReplicationRole::Server)
		return;
	WorldGuard guard(*this);
	managed->delegation.SetReleaseVeto(std::move(canRelease));
}
bool ReplicationManager::SetDelegationGroup(Replica3 &member, Replica3 *leader)
{
	if (role != ReplicationRole::Server)
		return false;
	WorldGuard guard(*this);
	return managed->delegation.SetGroup(member, leader);
}
bool ReplicationManager::DeclineAuthority(Replica3 &replica, bool declined)
{
	if (role != ReplicationRole::Client)
		return false;
	WorldGuard guard(*this);
	return managed->Decline(replica, declined);
}
std::optional<double> ReplicationManager::GetRenderTick(PeerGuid source, unsigned delay) const
{
	if (!managed)
		return std::nullopt;
	return managed->RenderTick(source, delay);
}
bool ReplicationManager::SetObserverStateBudget(PeerGuid observer, uint32_t bytesPerTick)
{
	WorldGuard guard(*this);
	return managed->ConfigureObserver(observer, nullptr, bytesPerTick);
}
bool ReplicationManager::SetUpdateInterval(PeerGuid observer, Replica3 &replica, unsigned ticks)
{
	WorldGuard guard(*this);
	return ticks && managed->ConfigureObserver(observer, &replica, ticks);
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
			managed->Receive(*packet);
		}
		return true;
	}
	if (!managed || role == ReplicationRole::Unconfigured)
		return false;
	switch (packet->data[0])
	{
	case ID_CONNECTION_REQUEST_ACCEPTED:
	case ID_NEW_INCOMING_CONNECTION:
	{
		WorldGuard guard(*this);
		managed->Connected(ToPeerGuid(packet->guid), packet->systemAddress, packet->data[0] == ID_NEW_INCOMING_CONNECTION);
		break;
	}
	case ID_CONNECTION_LOST:
	case ID_DISCONNECTION_NOTIFICATION:
	{
		{
			WorldGuard guard(*this);
			managed->Disconnected(ToPeerGuid(packet->guid));
		}
		DeliverDelegation();
		break;
	}
	}
	return false;
}

void ReplicationManager::Shutdown()
{
	if (managed)
		managed->Reset();
}
}
