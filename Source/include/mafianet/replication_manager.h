// Copyright (c) 2026, MafiaHub. MIT license.

#pragma once

#include "mafianet/export.h"

#include <memory>
#include <utility>

namespace MafiaNet
{
class RakPeer;
class RakPeerInterface;
class Replica3;
class ReplicaManager3;
class NetworkIDManager;
struct Packet;
enum PluginReceiveResult : int;

enum class ReplicationTickMode
{
	Automatic,
	Manual
};

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

	/// Select the original RM3 wire protocol and supply its connection/allocation callbacks.
	/// The returned manager belongs to the peer. Select a protocol before referencing objects.
	template <class Manager, class... Args>
	Manager *UseLegacyProtocol(Args &&...args)
	{
		if (engine || role != ReplicationRole::Unconfigured)
			return nullptr;
		auto manager = std::make_unique<Manager>(std::forward<Args>(args)...);
		auto *result = manager.get();
		Initialize(std::move(manager));
		legacy = true;
		return result;
	}

	void SetTickMode(ReplicationTickMode mode) { tickMode = mode; }
	void Tick();

	bool SetRole(ReplicationRole role);
	ReplicationRole GetRole() const { return role; }
	bool Reference(Replica3 *replica);
	void Dereference(Replica3 *replica);
	unsigned int GetReplicaCount() const;

private:
	friend class RakPeer;
	explicit ReplicationManager(RakPeerInterface &peer);
	void Shutdown();

	void Initialize(std::unique_ptr<ReplicaManager3> manager);
	PluginReceiveResult ProcessPacket(Packet *packet);
	void ReceiveTick();
	RakPeerInterface &peer;
	std::unique_ptr<NetworkIDManager> ids;
	std::unique_ptr<ReplicaManager3> engine;
	bool legacy = false;
	ReplicationTickMode tickMode = ReplicationTickMode::Automatic;
	ReplicationRole role = ReplicationRole::Unconfigured;
};
}
