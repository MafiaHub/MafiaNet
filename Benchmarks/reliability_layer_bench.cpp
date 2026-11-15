/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The send -> datagram -> receive -> ack path of two ReliabilityLayer
 *  instances joined in memory (the same wiring as the hermetic unit tests),
 *  so the numbers cover packing, splitting, reassembly and ack processing
 *  without the OS socket.
 */

#include <benchmark/benchmark.h>

#include <vector>

#include "mafianet/ds_list.h"
#include "mafianet/get_time.h"
#include "mafianet/mtu_size.h"
#include "mafianet/packet_priority.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/rand.h"
#include "mafianet/reliability_layer.h"
#include "mafianet/session_crypto.h"
#include "mafianet/socket2.h"

using namespace MafiaNet;

namespace
{
struct QueueSocket : public RakNetSocket2
{
	std::vector<std::vector<char>> sent;

	RNS2SendResult Send(RNS2_SendParameters *sendParameters, const char *, unsigned int) override
	{
		sent.emplace_back(sendParameters->data, sendParameters->data + sendParameters->length);
		return sendParameters->length;
	}
};

struct LayerPair
{
	ReliabilityLayer a, b;
	QueueSocket aSock, bSock;
	SystemAddress aAddr{"127.0.0.1", 40001}, bAddr{"127.0.0.1", 40002};
	DataStructures::List<PluginInterface2 *> handlers;
	RakNetRandom rnr;
	BitStream updateBitStream{MAXIMUM_MTU_SIZE};
	CCTimeType now = GetTimeUS();

	// With \a encrypted, both layers get a matching session key set, so every
	// datagram is sealed and opened; otherwise the layers run the plaintext path
	// (still reserving the cipher overhead, as a peer in Required mode would).
	explicit LayerPair(bool encrypted = false)
	{
		a.Reset(true, MAXIMUM_MTU_SIZE);
		b.Reset(true, MAXIMUM_MTU_SIZE);
		if (encrypted)
		{
			const RakNetGUID gi(1), gr(2);
			EphemeralKeyPair ei, er;
			const CryptInfo ci = ei.MakeCryptInfo();
			const CryptInfo cr = er.MakeCryptInfo();
			SessionKeys ki, kr;
			DeriveSessionKeys(ei, true, ci, cr, gi, gr, &ki);
			DeriveSessionKeys(er, false, ci, cr, gi, gr, &kr);
			a.SetSessionKeys(ki);
			b.SetSessionKeys(kr);
		}
	}

	// One 10ms tick in both directions; returns messages b completed.
	int Tick()
	{
		now += 10000;
		a.Update(&aSock, bAddr, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream);
		for (auto &d : aSock.sent)
			b.HandleSocketReceiveFromConnectedPlayer(d.data(), (unsigned int) d.size(), aAddr, handlers,
			                                         MAXIMUM_MTU_SIZE, &bSock, &rnr, now, updateBitStream);
		aSock.sent.clear();

		b.Update(&bSock, aAddr, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream);
		for (auto &d : bSock.sent)
			a.HandleSocketReceiveFromConnectedPlayer(d.data(), (unsigned int) d.size(), bAddr, handlers,
			                                         MAXIMUM_MTU_SIZE, &aSock, &rnr, now, updateBitStream);
		bSock.sent.clear();

		int received = 0;
		unsigned char *data;
		while (b.Receive(&data) > 0)
		{
			rakFree_Ex(data, _FILE_AND_LINE_);
			++received;
		}
		return received;
	}
};
}

// Many small reliable-ordered messages per tick: packing and ack throughput.
static void SmallReliableOrdered(benchmark::State &state, bool encrypted)
{
	LayerPair pair(encrypted);
	std::vector<char> message(32, 7);
	message[0] = (char) 200;
	const int perTick = (int) state.range(0);
	int64_t delivered = 0;
	for (auto _ : state)
	{
		for (int i = 0; i < perTick; ++i)
			pair.a.Send(message.data(), BYTES_TO_BITS((BitSize_t) message.size()), Priority::High,
			            Reliability::ReliableOrdered, 0, true, MAXIMUM_MTU_SIZE, pair.now, 0);
		delivered += pair.Tick();
	}
	state.SetItemsProcessed(delivered);
	state.SetBytesProcessed(delivered * (int64_t) message.size());
}
static void ReliabilityLayer_SmallReliableOrdered(benchmark::State &state)
{
	SmallReliableOrdered(state, false);
}
BENCHMARK(ReliabilityLayer_SmallReliableOrdered)->Arg(16)->Arg(256);

static void ReliabilityLayer_SmallReliableOrderedEncrypted(benchmark::State &state)
{
	SmallReliableOrdered(state, true);
}
BENCHMARK(ReliabilityLayer_SmallReliableOrderedEncrypted)->Arg(16)->Arg(256);

// One large message per iteration: split and reassembly.
static void SplitMessage(benchmark::State &state, bool encrypted)
{
	LayerPair pair(encrypted);
	std::vector<char> message((size_t) state.range(0), 9);
	message[0] = (char) 200;
	for (auto _ : state)
	{
		pair.a.Send(message.data(), BYTES_TO_BITS((BitSize_t) message.size()), Priority::Medium,
		            Reliability::ReliableOrdered, 0, true, MAXIMUM_MTU_SIZE, pair.now, 0);
		int got = 0;
		for (int tick = 0; got == 0 && tick < 10000; ++tick)
			got = pair.Tick();
		if (got == 0)
			state.SkipWithError("message not delivered");
	}
	state.SetBytesProcessed(state.iterations() * state.range(0));
}

static void ReliabilityLayer_SplitMessage(benchmark::State &state)
{
	SplitMessage(state, false);
}
BENCHMARK(ReliabilityLayer_SplitMessage)->Arg(16 * 1024)->Arg(256 * 1024);

static void ReliabilityLayer_SplitMessageEncrypted(benchmark::State &state)
{
	SplitMessage(state, true);
}
BENCHMARK(ReliabilityLayer_SplitMessageEncrypted)->Arg(16 * 1024)->Arg(256 * 1024);
