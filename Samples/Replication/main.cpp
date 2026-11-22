// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/peer_handle.h"
#include "mafianet/snapshot_replica.h"
#include <chrono>
#include <cstdio>
#include <thread>

using namespace MafiaNet;

namespace
{
struct VehicleState
{
	int speed = 0;
	void Write(BitStream &out) const { out.Write(speed); }
	bool Read(BitStream &in) { return in.Read(speed); }
};
struct DriverInput
{
	int speed = 0;
	void Write(BitStream &out) const { out.Write(speed); }
	bool Read(BitStream &in) { return in.Read(speed); }
};
struct Vehicle : SnapshotReplica<VehicleState>
{
	bool Drive(PeerGuid, const DriverInput &input)
	{
		if (input.speed < 0 || input.speed > 120)
			return false;
		state.speed = input.speed;
		return true;
	}
};
bool Setup(Peer &peer, ReplicationRole role)
{
	auto &replication = peer->GetReplication();
	SocketDescriptor socket(0, "127.0.0.1");
	if (!replication.SetRole(role) || !replication.RegisterReplica<Vehicle>(ReplicaTypeId{1}, 1) ||
	    !replication.RegisterOwnerUpdate<Vehicle, DriverInput>(1, &Vehicle::Drive) ||
	    peer->Startup(2, &socket, 1) != RAKNET_STARTED)
		return false;
	if (role == ReplicationRole::Server)
		peer->SetMaximumIncomingConnections(2);
	return true;
}
}

int main()
{
	Peer server, driver, spectator;
	if (!Setup(server, ReplicationRole::Server) || !Setup(driver, ReplicationRole::Client) || !Setup(spectator, ReplicationRole::Client))
		return 1;
	const auto port = server->GetInternalID().GetPort();
	for (auto *client : {&driver, &spectator})
		if ((*client)->Connect("127.0.0.1", port, nullptr, 0) != CONNECTION_ATTEMPT_STARTED)
			return 1;
	auto &host = server->GetReplication();
	auto &grid = host.GetInterestGrid();
	const auto first = ToPeerGuid(driver->GetMyGUID()), second = ToPeerGuid(spectator->GetMyGUID());
	Vehicle vehicle;
	if (!grid.Configure({{0, 0}, {256, 256}, 64}) || !grid.SetObserverRadius(first, {63, 20}, 10, 15) ||
	    !grid.SetObserverCells(second, {{1, 0}}) || !host.Reference(&vehicle, SpatialScope{{72, 20}}))
		return 1;
	const auto id = vehicle.GetNetworkID();
	auto copy = [&](Peer &peer)
	{ return static_cast<Vehicle *>(peer->GetReplication().FindReplica(id)); };
	auto wait = [&](auto ready)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			for (auto *peer : {&server, &driver, &spectator})
				for ([[maybe_unused]] auto packet : peer->incoming())
				{
				}
			if (ready())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return false;
	};
	if (!wait([&]
	          { return copy(driver) && copy(spectator); }) ||
	    !host.SetAuthority(vehicle, first))
		return 1;
	if (!wait([&]
	          { return driver->GetReplication().HasLocalAuthority(*copy(driver)); }) ||
	    !driver->GetReplication().SendOwnerUpdate(*copy(driver), DriverInput{45}))
		return 1;
	if (!wait([&]
	          { return copy(spectator)->state.speed == 45; }))
		return 1;
	std::puts("The spectator sees the driver's accepted speed of 45.");
	if (!grid.SetPosition(id, {100, 20}) || !host.SetAuthority(vehicle, second))
		return 1;
	if (!wait([&]
	          { return !copy(driver) && copy(spectator) && spectator->GetReplication().HasLocalAuthority(*copy(spectator)); }))
		return 1;
	if (!spectator->GetReplication().SendOwnerUpdate(*copy(spectator), DriverInput{60}) || !wait([&]
	                                                                                             { return vehicle.state.speed == 60; }))
		return 1;
	std::puts("The second driver takes control. The first driver has left streaming range.");
	grid.SetObserverWorld(second, 0, 1);
	if (!wait([&]
	          { return !copy(spectator); }) ||
	    !host.Destroy(&vehicle))
		return 1;
	std::puts("Changing room releases the observer's copy and revokes its grant.");
	return 0;
}
