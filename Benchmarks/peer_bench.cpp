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
 *  Only Send(), Receive() and DeallocatePacket() are used on the peers, so the
 *  file builds unchanged against older trees for A/B comparisons.
 */

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/packet_priority.h"
#include "mafianet/peer.h"

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
BENCHMARK(RakPeer_SendReceiveSmall)->Arg(64)->Arg(512);

// Split-size messages: splitting and reassembly through the whole peer.
static void RakPeer_SendReceiveLarge(benchmark::State &state)
{
	SendReceive(state, (int) state.range(0), 4);
}
BENCHMARK(RakPeer_SendReceiveLarge)->Arg(4 * 1024)->Arg(64 * 1024);
