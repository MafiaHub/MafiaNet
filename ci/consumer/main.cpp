// Links against an installed MafiaNet and exercises it once: create a peer,
// start it (which initialises libsodium), shut it down. Exit code 0 on success.
#include "mafianet/remote_procedure_call.h"
#include "mafianet/snapshot_replica.h"
#include "mafianet/peer_interface.h"

#include <cstdint>
#include <cstdio>

struct State
{
	bool active{};
	void Write(MafiaNet::BitStream &out) const { out.Write(active); }
	bool Read(MafiaNet::BitStream &in) { return in.Read(active); }
};
struct Entity : MafiaNet::SnapshotReplica<State> {};

int main()
{
	MafiaNet::RakPeerInterface *peer = MafiaNet::RakPeerInterface::GetInstance();
	if (peer == nullptr)
		return 1;
	// Exported data, not just functions: on Windows these need dllimport (MAFIANET_DATA_API).
	const MafiaNet::AddressOrGUID unassigned;
	if (!unassigned.IsUndefined() || MafiaNet::UNASSIGNED_RAKNET_GUID.g != (uint64_t) -1)
	{
		std::printf("UNASSIGNED_* globals have unexpected values\n");
		return 1;
	}
	MafiaNet::SocketDescriptor descriptor;
	const MafiaNet::StartupResult result = peer->Startup(1, &descriptor, 1);
	const bool registered = peer->GetRPC().RegisterFunction("probe", [](MafiaNet::BitStream *, MafiaNet::Packet *, void *) {}, nullptr);
	auto &service = peer->GetReplication();
	Entity entity;
	const bool replication = service.SetRole(MafiaNet::ReplicationRole::Server) &&
	    service.RegisterReplica<Entity>(MafiaNet::ReplicaTypeId{1}, 1) &&
	    service.GetInterestGrid().Configure({{0, 0}, {32, 32}, 8}) &&
	    service.Reference(&entity, MafiaNet::SpatialScope{{1, 1}}) && service.Destroy(&entity);
	peer->Shutdown(0);
	MafiaNet::RakPeerInterface::DestroyInstance(peer);
	if (result != MafiaNet::RAKNET_STARTED)
	{
		std::printf("Startup failed: %d\n", (int) result);
		return 1;
	}
	if (!registered || !replication)
		return 1;
	std::printf("MafiaNet consumer OK\n");
	return 0;
}
