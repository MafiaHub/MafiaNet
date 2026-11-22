// Copyright (c) 2026, MafiaHub. MIT license.

#pragma once

#include "mafianet/export.h"

#include <memory>

namespace MafiaNet
{
class RakPeer;
class RakPeerInterface;
class Replica3;

enum class ReplicationRole
{
	Unconfigured,
	Server,
	Client
};

/// Per-peer object replication. The service and its configuration outlive a
/// shutdown/startup cycle. Session objects and connections are cleared on shutdown.
class RAK_DLL_EXPORT ReplicationManager final
{
public:
	~ReplicationManager();
	ReplicationManager(const ReplicationManager &) = delete;
	ReplicationManager &operator=(const ReplicationManager &) = delete;

	bool SetRole(ReplicationRole role);
	ReplicationRole GetRole() const { return role; }
	bool Reference(Replica3 *replica);
	void Dereference(Replica3 *replica);
	unsigned int GetReplicaCount() const;

private:
	friend class RakPeer;
	explicit ReplicationManager(RakPeerInterface &peer);
	void Shutdown();

	class Engine;
	RakPeerInterface &peer;
	std::unique_ptr<Engine> engine;
	ReplicationRole role = ReplicationRole::Unconfigured;
};
}
