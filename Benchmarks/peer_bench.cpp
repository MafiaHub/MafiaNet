/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Two connected RakPeers on the simulated network (Tests/Support): no
 *  sockets, no threads, a virtual clock. Unlike the bare ReliabilityLayer
 *  benchmark this covers RakPeer's own per-message work: the buffered send
 *  command each Send() queues for the update cycle, and the Packet each
 *  received message becomes and DeallocatePacket() frees.
 *
 *  The churn benchmark measures a visitor's whole connection lifecycle and
 *  optional GetSystemList() calls against a stable population of peers.
 */

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/packet_priority.h"
#include "mafianet/peer.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;

namespace
{
struct ConnectedPair
{
	SimulatedNetwork net;
	RakPeer *client = nullptr;
	RakPeer *server = nullptr;
	SystemAddress serverAddress;
	bool connected = false;

	ConnectedPair()
	{
		client = net.AddPeer(1);
		server = net.AddPeer(1, 1);
		serverAddress = net.AddressOf(server);
		connected = net.Connect(client, server);
		// The benchmark drains the server itself, so the cost measured is
		// Receive() + DeallocatePacket(), not the network's inbox copies.
		net.SetReceiveEnabled(server, false);
		DrainServer(nullptr);
		net.Inbox(client).clear();
		net.Inbox(server).clear();
	}

	// Frees everything the server has received; counts user messages.
	int64_t DrainServer(int64_t *bytes)
	{
		int64_t messages = 0;
		for (Packet *packet = server->Receive(); packet; packet = server->Receive())
		{
			if (packet->length > 0 && packet->data[0] == ID_USER_PACKET_ENUM)
			{
				++messages;
				if (bytes)
					*bytes += packet->length;
			}
			server->DeallocatePacket(packet);
		}
		return messages;
	}

	// Steps the network 1 ms at a time until the server has received `count`
	// user messages, or a simulated-time bound passes. Returns how many arrived.
	int64_t DeliverAll(int64_t count, int64_t *bytes)
	{
		int64_t received = DrainServer(bytes);
		for (int ms = 0; received < count && ms < 60000; ++ms)
		{
			net.Step(1);
			received += DrainServer(bytes);
		}
		net.Inbox(client).clear();
		return received;
	}
};

void SendReceive(benchmark::State &state, int messageBytes, int messagesPerIteration)
{
	ConnectedPair pair;
	if (!pair.connected)
	{
		state.SkipWithError("peers did not connect");
		return;
	}
	std::vector<char> message((size_t) messageBytes, 7);
	message[0] = (char) ID_USER_PACKET_ENUM;
	int64_t delivered = 0;
	int64_t bytes = 0;
	// Settle handshake traffic before measuring application datagrams.
	pair.net.Step(100);
	int64_t forwardBytes = 0, reverseBytes = 0, forwardPackets = 0, reversePackets = 0;
	pair.net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *, int length)
	                {
		if (from == pair.serverAddress)
		{
			reverseBytes += length;
			++reversePackets;
		}
		else
		{
			forwardBytes += length;
			++forwardPackets;
		} });
	for (auto _ : state)
	{
		for (int i = 0; i < messagesPerIteration; ++i)
			pair.client->Send(message.data(), messageBytes, Priority::High, Reliability::ReliableOrdered, 0,
			                  pair.serverAddress, false);
		const int64_t got = pair.DeliverAll(messagesPerIteration, &bytes);
		if (got != messagesPerIteration)
		{
			state.SkipWithError("messages not delivered");
			break;
		}
		delivered += got;
	}
	// Include delayed ACKs in the totals, outside the timed send/receive loop.
	if (!pair.net.RunUntil([&]
	                       {
		    RakNetStatistics stats;
		    return pair.client->GetStatistics(pair.serverAddress, &stats) && stats.messagesInResendBuffer == 0; }, 5000, 1))
		state.SkipWithError("final acknowledgements did not arrive");
	pair.net.SetTap({});
	const auto avg = benchmark::Counter::kAvgIterations;
	state.counters["forward_udp_bytes"] = benchmark::Counter(forwardBytes, avg);
	state.counters["reverse_udp_bytes"] = benchmark::Counter(reverseBytes, avg);
	state.counters["forward_datagrams"] = benchmark::Counter(forwardPackets, avg);
	state.counters["reverse_datagrams"] = benchmark::Counter(reversePackets, avg);
	if (bytes)
		state.counters["udp_bytes_per_delivered_byte"] = double(forwardBytes + reverseBytes) / bytes;
	state.SetItemsProcessed(delivered);
	state.SetBytesProcessed(bytes);
}
}

// Many small reliable-ordered messages per iteration: per-Send command and
// per-received-Packet allocation dominate the RakPeer share of the cost.
static void RakPeer_SendReceiveSmall(benchmark::State &state)
{
	SendReceive(state, 64, (int) state.range(0));
}
BENCHMARK(RakPeer_SendReceiveSmall)->Arg(64)->Arg(512)->Iterations(64)->UseRealTime();

// Split-size messages: splitting and reassembly through the whole peer.
static void RakPeer_SendReceiveLarge(benchmark::State &state)
{
	SendReceive(state, (int) state.range(0), 4);
}
BENCHMARK(RakPeer_SendReceiveLarge)->Arg(4 * 1024)->Arg(64 * 1024)->Iterations(64)->UseRealTime();

// One visitor connects and disconnects while other clients stay connected. Vary
// the queries per visit to distinguish the cost of publishing from copying a list.
static void RakPeer_SystemListChurn(benchmark::State &state)
{
	const auto residents = static_cast<unsigned int>(state.range(0));
	SimulatedNetwork net;
	auto *server = net.AddPeer(residents + 1, residents + 1);
	for (unsigned int i = 0; i < residents; ++i)
	{
		if (!net.Connect(net.AddPeer(1), server))
		{
			state.SkipWithError("resident did not connect");
			return;
		}
	}
	auto *visitor = net.AddPeer(1);
	const auto serverAddress = net.AddressOf(server);
	const auto visitorAddress = net.AddressOf(visitor);
	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	for (auto _ : state)
	{
		net.Inbox(server).clear();
		net.Inbox(visitor).clear();
		if (!net.Connect(visitor, server))
		{
			state.SkipWithError("visitor did not connect");
			break;
		}
		for (int i = 0; i < state.range(1); ++i)
			server->GetSystemList(addresses, guids);
		server->CloseConnection(visitorAddress, false);
		visitor->CloseConnection(serverAddress, false);
		net.Step(1);
	}
}
BENCHMARK(RakPeer_SystemListChurn)->Args({1, 0})->Args({1, 32})->Args({32, 0})->Args({32, 32});

// Each iteration measures a fixed two-second stream of 100 probes. Bound the
// background backlog by refilling only as previously queued bytes arrive.
static void RakPeer_UrgentLatency(benchmark::State &state)
{
	const bool withBulk = state.range(0) != 0;
	ConnectedPair pair;
	if (!pair.connected)
	{
		state.SkipWithError("connection failed");
		return;
	}
	pair.client->SetPerConnectionOutgoingBandwidthLimit(2 * 1024 * 1024);
	pair.net.SetReceiveEnabled(pair.server, true);
	std::array<char, 900> bulk{};
	bulk[0] = ID_USER_PACKET_ENUM + 1;
	std::array<char, 2> probe{static_cast<char>(ID_USER_PACKET_ENUM), 0};
	for (auto _ : state)
	{
		int bulkSent = 0, bulkReceived = 0;
		std::vector<int> delays;
		constexpr int probes = 100;
		constexpr int periodMs = 20;
		int bulkDuringWindow = 0;
		for (int ms = 0; ms < 10000; ++ms)
		{
			if (ms < probes * periodMs)
			{
				while (withBulk && bulkSent - bulkReceived < 64)
				{
					if (!pair.client->Send(bulk.data(), bulk.size(), Priority::Low, Reliability::ReliableOrdered, 1, pair.serverAddress, false))
					{
						state.SkipWithError("bulk send failed");
						return;
					}
					++bulkSent;
				}
				probe[1] = static_cast<char>(ms / periodMs);
				if (ms % periodMs == 0 && !pair.client->Send(probe.data(), probe.size(), Priority::High, Reliability::ReliableOrdered, 0, pair.serverAddress, false))
				{
					state.SkipWithError("probe send failed");
					return;
				}
			}
			pair.net.Step(1);
			for (const auto &msg : pair.net.Inbox(pair.server))
			{
				if (msg.id == ID_USER_PACKET_ENUM)
				{
					if (msg.data.size() != probe.size() || msg.data[1] != delays.size())
					{
						state.SkipWithError("unexpected probe sequence");
						return;
					}
					delays.push_back(ms + 1 - msg.data[1] * periodMs);
				}
				else if (msg.id == ID_USER_PACKET_ENUM + 1)
					++bulkReceived;
			}
			pair.net.Inbox(pair.server).clear();
			if (ms + 1 == probes * periodMs)
				bulkDuringWindow = bulkReceived;
			if (ms + 1 >= probes * periodMs && delays.size() == probes && bulkSent == bulkReceived)
				break;
		}
		if (delays.size() != probes || bulkReceived != bulkSent || (withBulk && !bulkDuringWindow))
		{
			state.SkipWithError("traffic did not make progress or drain");
			return;
		}
		std::sort(delays.begin(), delays.end());
		state.counters["probe_p50_ms"] = delays[49];
		state.counters["probe_p95_ms"] = delays[94];
		state.counters["probe_p99_ms"] = delays[98];
		state.counters["probes_delivered"] = delays.size();
		state.counters["bulk_B_per_sim_s"] = bulkDuringWindow * bulk.size() * 1000.0 / (probes * periodMs);
	}
}
BENCHMARK(RakPeer_UrgentLatency)->ArgName("bulk")->Arg(0)->Arg(1)->Iterations(1);
