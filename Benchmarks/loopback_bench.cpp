/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <thread>
#include "mafianet/message_identifiers.h"
#include "mafianet/mmsg_batch.h"
#include "mafianet/peer_handle.h"

using namespace MafiaNet;
namespace
{
using Clock = std::chrono::steady_clock;

void RakPeer_LoopbackRate(benchmark::State &state)
{
	constexpr int burst = MMSG_BATCH_MAX * 2;
	std::array<char, 900> message{};
	message[0] = static_cast<char>(ID_USER_PACKET_ENUM);
	Peer server;
	SocketDescriptor sd(0, "127.0.0.1");
	if (server->Startup(1, &sd, 1) != RAKNET_STARTED)
	{
		state.SkipWithError("server startup failed");
		return;
	}
	server->SetMaximumIncomingConnections(1);
	Peer client;
	const SystemAddress address("127.0.0.1", server->GetInternalID().GetPort());
	if (client->Startup(1, &sd, 1) != RAKNET_STARTED || client->Connect("127.0.0.1", address.GetPort(), nullptr, 0) != CONNECTION_ATTEMPT_STARTED)
	{
		state.SkipWithError("client startup/connect failed");
		return;
	}
	bool accepted = false, incoming = false;
	const auto deadline = Clock::now() + std::chrono::seconds(10);
	while (!(accepted && incoming) && Clock::now() < deadline)
	{
		for (auto packet : client.incoming())
			accepted |= packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED;
		for (auto packet : server.incoming())
		{
			if (packet.id() == ID_SESSION_CONFIG_REQUEST)
				server->AcceptSession(packet->guid, nullptr, 0);
			incoming |= packet.id() == ID_NEW_INCOMING_CONNECTION;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (!(accepted && incoming))
	{
		state.SkipWithError("handshake timed out");
		return;
	}

	const int burstsPerSecond = static_cast<int>(state.range(0));
	for (auto _ : state)
	{
		const auto start = Clock::now();
		int bursts = 0, received = 0;
		while (received < burstsPerSecond * burst)
		{
			const auto elapsed = Clock::now() - start;
			if (elapsed > std::chrono::seconds(15))
			{
				state.SkipWithError("loopback transfer timed out");
				return;
			}
			if (bursts < burstsPerSecond && elapsed >= std::chrono::microseconds(1000000) * bursts / burstsPerSecond)
			{
				for (int i = 0; i < burst; ++i)
					if (!client->Send(message.data(), message.size(), Priority::High, Reliability::ReliableOrdered, 0, address, false))
					{
						state.SkipWithError("send rejected");
						return;
					}
				++bursts;
			}
			for (auto packet : server.incoming())
				if (packet->data[0] == ID_USER_PACKET_ENUM && packet->length == message.size())
					++received;
			if (received < burstsPerSecond * burst)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		// Include the last pacing interval at rates the receiver can sustain.
		std::this_thread::sleep_until(start + std::chrono::seconds(1));
	}
	const int bytes = burstsPerSecond * burst * message.size();
	state.counters["offered_B_per_s"] = bytes;
	state.SetBytesProcessed(state.iterations() * bytes);
}
BENCHMARK(RakPeer_LoopbackRate)->ArgName("bursts_per_s")->Arg(10)->Arg(50)->Arg(100)->MeasureProcessCPUTime()->UseRealTime()->Iterations(1);
}
