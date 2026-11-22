// Copyright (c) 2026, MafiaHub. MIT license.

#pragma once

#include "mafianet/export.h"
#include "mafianet/bit_stream.h"
#include "mafianet/interest_grid.h"

#include "mafianet/types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <vector>
#include "mafianet/time.h"
#include <utility>

namespace MafiaNet
{
class RakPeer;
class RakPeerInterface;
class Replica3;
class NetworkIDManager;
class ManagedReplication;
class RM3SerializeExecutor;
enum class ReplicaTypeId : uint32_t
{
};
enum class ReplicationPeerStatus
{
	Waiting,
	Active,
	SchemaMismatch,
	ProtocolMismatch
};
struct Packet;

enum class ReplicationTickMode
{
	Automatic,
	Manual
};

enum class ReplicationRole
{
	Unconfigured,
	Server,
	Client,
	Peer
};

/// Per-peer object replication. The service and its configuration outlive a
/// shutdown/startup cycle. Session objects and connections are cleared on shutdown.
class RAK_DLL_EXPORT ReplicationManager final
{
public:
	~ReplicationManager();
	ReplicationManager(const ReplicationManager &) = delete;
	ReplicationManager &operator=(const ReplicationManager &) = delete;

	void SetTickMode(ReplicationTickMode mode) { tickMode = mode; }
	void Tick();

	/// Register the same numeric ID and codec version at both ends before connecting.
	template <class T>
	bool RegisterReplica(ReplicaTypeId id, uint16_t version)
	{
		return RegisterReplica<T>(id, version, []
		                          { return new T(); }, [](T *object)
		                          { delete object; });
	}
	template <class T, class Create, class Release>
	bool RegisterReplica(ReplicaTypeId id, uint16_t version, Create create, Release release)
	{
		return RegisterType(id, version, typeid(T), std::move(create),
		                    [release = std::move(release)](Replica3 *object) mutable
		                    { release(static_cast<T *>(object)); });
	}
	/// One upstream report format per replica type. The server decides which fields to apply.
	template <class T, class Report>
	bool RegisterOwnerUpdate(uint16_t version, bool (T::*accept)(PeerGuid, const Report &))
	{
		if (!accept)
			return false;
		return RegisterReport(typeid(T), typeid(Report), version, [accept](Replica3 &object, PeerGuid sender, BitStream &in)
		                      {
			Report report{};
			return report.Read(in) && !in.GetNumberOfUnreadBits() && (static_cast<T&>(object).*accept)(sender,report); });
	}
	bool SetAuthority(Replica3 &replica, PeerGuid owner);
	bool RevokeAuthority(Replica3 &replica);
	bool CorrectAuthority(Replica3 &replica);
	bool HasLocalAuthority(Replica3 &replica) const;
	template <class Report>
	bool SendOwnerUpdate(Replica3 &replica, const Report &report)
	{
		BitStream out;
		report.Write(out);
		return SendReport(replica, typeid(Report), out);
	}
	Replica3 *FindReplica(NetworkID id) const;
	bool Destroy(Replica3 *replica);
	bool ReleaseRemote(NetworkID id);
	class RAK_DLL_EXPORT WorldGuard
	{
	public:
		explicit WorldGuard(ReplicationManager &manager);

	private:
		std::unique_lock<std::recursive_mutex> lock;
	};
	ReplicationPeerStatus GetPeerStatus(PeerGuid guid) const;
	void SetAutomaticAdmission(bool enabled);
	bool Admit(PeerGuid guid);

	bool SetRole(ReplicationRole role);
	ReplicationRole GetRole() const { return role; }
	bool Reference(Replica3 *replica);
	bool Reference(Replica3 *replica, SpatialScope scope);
	InterestGrid &GetInterestGrid();
	bool ReferenceStatic(Replica3 *replica, NetworkID id);
	void Dereference(Replica3 *replica);
	unsigned int GetReplicaCount() const;
	unsigned int GetConnectionCount() const;
	Replica3 *GetReplicaAtIndex(unsigned int index) const;
	void GetLocalReplicas(std::vector<Replica3 *> &out) const;
	bool SetObserverVirtualWorld(PeerGuid guid, uint32_t dimension);
	void SetSerializeInterval(Time interval);
	void SetSerializeExecutor(RM3SerializeExecutor *executor);
	void SetSerializationList(std::function<bool(PeerGuid, std::vector<Replica3 *> &)> query);
	void SetTickCallbacks(std::function<void()> before, std::function<void()> after);
	void SetConnectionCallback(std::function<void(PeerGuid, bool)> callback);

private:
	friend class RakPeer;
	explicit ReplicationManager(RakPeerInterface &peer);
	void Shutdown();

	bool RegisterType(ReplicaTypeId id, uint16_t version, std::type_index type, std::function<Replica3 *()> create, std::function<void(Replica3 *)> release);
	bool RegisterReport(std::type_index object, std::type_index report, uint16_t version, std::function<bool(Replica3 &, PeerGuid, BitStream &)> accept);
	bool SendReport(Replica3 &object, std::type_index type, const BitStream &payload);
	ManagedReplication &Managed();

	bool ProcessPacket(Packet *packet);
	void ReceiveTick();
	RakPeerInterface &peer;
	std::unique_ptr<NetworkIDManager> ids;
	std::unique_ptr<ManagedReplication> managed;
	ReplicationTickMode tickMode = ReplicationTickMode::Automatic;
	ReplicationRole role = ReplicationRole::Unconfigured;
};
}
