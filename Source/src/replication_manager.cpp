// Copyright (c) 2026, MafiaHub. MIT license.

#include "mafianet/replication_manager.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/replica_manager3.h"

namespace MafiaNet
{
class ReplicationManager::Engine final : public ReplicaManager3
{
public:
	explicit Engine(RakPeerInterface &peer)
	{
		SetRakPeerInterface(&peer);
		SetNetworkIDManager(&ids);
	}

	// Connections are enrolled only after peers agree on their replication protocol.
	Connection_RM3 *AllocConnection(const SystemAddress &, RakNetGUID) const override { return nullptr; }
	void DeallocConnection(Connection_RM3 *connection) const override { delete connection; }
	using ReplicaManager3::OnRakPeerShutdown;

private:
	NetworkIDManager ids;
};

ReplicationManager::ReplicationManager(RakPeerInterface &peer)
    : peer(peer)
{
}

ReplicationManager::~ReplicationManager() = default;

bool ReplicationManager::SetRole(ReplicationRole requested)
{
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
		engine = std::make_unique<Engine>(peer);
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

void ReplicationManager::Shutdown()
{
	if (engine)
		engine->OnRakPeerShutdown();
}
}
