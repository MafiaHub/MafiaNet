// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/peer_handle.h"
#include "mafianet/snapshot_replica.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

using namespace MafiaNet;
namespace
{
struct VehicleState
{
	float position = 72, speed = 0;
	void Write(BitStream &out) const
	{
		out.Write(position);
		out.Write(speed);
	}
	bool Read(BitStream &in) { return in.Read(position) && in.Read(speed); }
};
struct DriverInput
{
	int throttle = 0;
	void Write(BitStream &out) const { out.Write(throttle); }
	bool Read(BitStream &in) { return in.Read(throttle); }
};
// One command advances one fixed simulation step. Both sides use the same game code.
void Simulate(VehicleState &state, const DriverInput &input)
{
	state.speed = std::clamp(state.speed + input.throttle * 2.0f, 0.0f, 30.0f);
	state.position += state.speed / 30.0f;
}
struct Vehicle : SnapshotReplica<VehicleState, 16>
{
	bool Drive(PeerGuid, const DriverInput &input)
	{
		if (input.throttle < -1 || input.throttle > 1)
			return false;
		Simulate(state, input);
		return true;
	}
	OwnerInputHistory<DriverInput> inputs;
	VehicleState predicted;
};
bool Setup(Peer &peer, ReplicationRole role)
{
	auto &replication = peer->GetReplication();
	SocketDescriptor socket(0, "127.0.0.1");
	if (!replication.SetRole(role) || !replication.RegisterReplica<Vehicle>(ReplicaTypeId{1}, 1) ||
	    !replication.RegisterOwnerUpdate<Vehicle, DriverInput>(1, &Vehicle::Drive) ||
	    peer->Startup(2, &socket, 1) != RAKNET_STARTED)
		return false;
	peer->ApplyNetworkSimulator(0, 35, 10);
	if (role == ReplicationRole::Server)
	{
		peer->SetMaximumIncomingConnections(2);
		replication.SetTickMode(ReplicationTickMode::Manual);
		replication.SetSerializeInterval(0);
	}
	return true;
}
}

int main(int argc, char **argv)
{
	Vehicle vehicle;
	Peer server, first, second;
	std::array<Peer *, 2> players{&first, &second};
	if (!Setup(server, ReplicationRole::Server))
		return 1;
	auto &host = server->GetReplication();
	auto &grid = host.GetInterestGrid();
	if (!grid.Configure({{0, 0}, {256, 256}, 64}) || !host.Reference(&vehicle, SpatialScope{{72, 20}}))
		return 1;
	auto id = vehicle.GetNetworkID();
	for (auto *player : players)
	{
		if (!Setup(*player, ReplicationRole::Client) ||
		    (*player)->Connect("127.0.0.1", server->GetInternalID().GetPort(), nullptr, 0) != CONNECTION_ATTEMPT_STARTED)
			return 1;
		// Both can see the road; handing off the grant changes who can drive.
		grid.SetObserverCells(ToPeerGuid((*player)->GetMyGUID()), {{0, 0}, {1, 0}, {2, 0}, {3, 0}});
		auto &view = (*player)->GetReplication();
		view.SetReplicaCallback([&view, id](const ReplicaEvent &event)
		                        {
			auto *car=static_cast<Vehicle *>(view.FindReplica(id));
			if(event.id!=id || !car) return;
			if(event.kind==ReplicaEventKind::InputProcessed)
				car->predicted=car->inputs.Reconcile(event.input,car->state,Simulate);
			else if(event.kind==ReplicaEventKind::AuthorityGranted || event.kind==ReplicaEventKind::AuthorityCorrected || event.kind==ReplicaEventKind::AuthorityLost)
			{
				car->inputs.Clear();
				car->predicted=car->state;
			} });
	}
	Time tick = 0;
	auto frame = [&]
	{
		for (auto *peer : {&server, &first, &second})
			for ([[maybe_unused]] auto packet : peer->incoming())
			{
			}
		grid.SetPosition(id, {std::clamp(vehicle.state.position, 0.0f, 256.0f), 20});
		host.Tick(++tick);
		std::this_thread::sleep_for(std::chrono::milliseconds(33));
	};
	auto wait = [&](auto ready)
	{
		for (unsigned i = 0; i < 180 && !ready(); ++i)
			frame();
		return ready();
	};
	auto view = [&](unsigned i) -> ReplicationManager &
	{ return (*players[i])->GetReplication(); };
	auto copy = [&](unsigned i)
	{ return static_cast<Vehicle *>(view(i).FindReplica(id)); };
	if (!wait([&]
	          { return copy(0) && copy(1); }))
		return 1;
	unsigned driver = 0;
	auto grant = [&]
	{
		auto owner = ToPeerGuid((*players[driver])->GetMyGUID()), spectator = ToPeerGuid((*players[1 - driver])->GetMyGUID());
		return host.SetUpdateInterval(owner, vehicle, 1) && host.SetUpdateInterval(spectator, vehicle, 3) &&
		       host.SetObserverStateBudget(spectator, 256) && host.SetAuthority(vehicle, owner) &&
		       wait([&]
		            { return view(driver).HasLocalAuthority(*copy(driver)) && !view(1 - driver).HasLocalAuthority(*copy(1 - driver)); });
	};
	if (!grant())
		return 1;
	const bool interactive = argc > 1 && std::string(argv[1]) == "--drive";
	std::string script = "aahabt";
	if (interactive)
		std::puts("a accelerates, b brakes, h hands off, t teleports, q quits.");
	for (std::size_t step = 0;; ++step)
	{
		char command;
		if (interactive)
		{
			if (!(std::cin >> command) || command == 'q')
				break;
		}
		else
		{
			if (step == script.size())
				break;
			command = script[step];
		}
		if (command == 'h')
		{
			driver = 1 - driver;
			if (!grant())
				return 1;
		}
		else if (command == 't')
		{
			vehicle.state = {72, 0};
			vehicle.MarkTeleport();
			if (!host.CorrectAuthority(vehicle))
				return 1;
		}
		else if (command == 'a' || command == 'b')
		{
			DriverInput input{command == 'a' ? 1 : -1};
			for (unsigned i = 0; i < 4; ++i)
			{
				auto *car = copy(driver);
				if (!car->inputs.Send(view(driver), *car, input))
					return 1;
				Simulate(car->predicted, input);
				frame();
			}
		}
		else
			continue;
		frame();
		auto rendered = copy(1 - driver)->SampleAt(static_cast<double>(tick) - 4.0, [](VehicleState a, VehicleState b, double t)
		                                           {
			a.position+=static_cast<float>(t)*(b.position-a.position);
			return a; });
		std::printf("tick %llu  driver %u  predicted %.2f  server %.2f  spectator %.2f\n",
		            static_cast<unsigned long long>(tick), driver + 1, copy(driver)->predicted.position, vehicle.state.position, rendered.position);
	}
	if (!wait([&]
	          { return copy(driver)->inputs.Size() == 0 && copy(1 - driver)->state.position == vehicle.state.position; }))
		return 1;
	for (auto *player : players)
		(*player)->GetReplication().SetReplicaCallback({});
	return host.Destroy(&vehicle) ? 0 : 1;
}
