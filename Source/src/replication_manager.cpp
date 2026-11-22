// Copyright (c) 2026, MafiaHub. MIT license.

#include "mafianet/replication_manager.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/replica_manager3.h"

namespace MafiaNet
{
namespace
{
class ManagedEngine final : public ReplicaManager3
{
public:
	// Connections are enrolled only after peers agree on their replication protocol.
	Connection_RM3 *AllocConnection(const SystemAddress &, RakNetGUID) const override { return nullptr; }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }
};
}

ReplicationManager::ReplicationManager(RakPeerInterface &peer)
    : peer(peer)
{
}

ReplicationManager::~ReplicationManager() = default;

bool ReplicationManager::SetRole(ReplicationRole requested)
{
	if (legacy)
		return false;
	if (requested == role)
		return true;
	if (engine && (engine->GetReplicaCount() || engine->GetConnectionCount()))
		return false;
	role = requested;
	return true;
}

bool ReplicationManager::Reference(Replica3 *replica)
{
	if (role != ReplicationRole::Server || !replica || replica->WasReferenced())
		return false;
	if (!engine)
		Initialize(std::make_unique<ManagedEngine>());
	engine->Reference(replica);
	return true;
}

void ReplicationManager::Dereference(Replica3 *replica)
{
	if (engine && replica && replica->replicaManager == engine.get())
		engine->Dereference(replica);
}

unsigned int ReplicationManager::GetReplicaCount() const
{
	return engine ? engine->GetReplicaCount() : 0;
}

void ReplicationManager::Initialize(std::unique_ptr<ReplicaManager3> manager)
{
	manager->rakPeerInterface = &peer;
	if (!manager->GetNetworkIDManager())
	{
		ids = std::make_unique<NetworkIDManager>();
		manager->SetNetworkIDManager(ids.get());
	}
	engine = std::move(manager);
}

void ReplicationManager::Tick()
{
	if (engine)
		engine->Update();
}

void ReplicationManager::ReceiveTick()
{
	if (tickMode == ReplicationTickMode::Automatic)
		Tick();
}

PluginReceiveResult ReplicationManager::ProcessPacket(Packet *packet)
{
	if (!legacy || !packet->length)
		return RR_CONTINUE_PROCESSING;
	switch (packet->data[0])
	{
	case ID_CONNECTION_REQUEST_ACCEPTED:
	case ID_NEW_INCOMING_CONNECTION:
		engine->OnNewConnection(packet->systemAddress, packet->guid, packet->data[0] == ID_NEW_INCOMING_CONNECTION);
		return RR_CONTINUE_PROCESSING;
	case ID_CONNECTION_LOST:
	case ID_DISCONNECTION_NOTIFICATION:
		engine->OnClosedConnection(packet->systemAddress, packet->guid, packet->data[0] == ID_CONNECTION_LOST ? LCR_CONNECTION_LOST : LCR_DISCONNECTION_NOTIFICATION);
		return RR_CONTINUE_PROCESSING;
	default:
		return engine->OnReceive(packet);
	}
}

void ReplicationManager::Shutdown()
{
	if (engine)
		engine->OnRakPeerShutdown();
}
}
