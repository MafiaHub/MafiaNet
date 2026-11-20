// Copyright (c) 2026, MafiaHub. Licensed under the MIT-style license in license.txt.
#include <benchmark/benchmark.h>
#include <array>
#include <cstring>
#include "SimulatedNetwork.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
void RakPeer_SharedBottleneck(benchmark::State &state)
{
	const size_t flows = static_cast<size_t>(state.range(0));
	constexpr size_t blockBytes = 16 * 1024;
	SimulatedBottleneck path(51200, {{0, 256 * 1024}});
	SimulatedNetwork net(static_cast<uint32_t>(state.range(2)));
	auto *receiver = net.AddPeer(2, 2);
	std::array<RakPeer *, 2> senders{};
	SimulatedNetwork::LinkRule rule;
	rule.delayMs = 50;
	for (size_t flow = 0; flow < flows; ++flow)
	{
		senders[flow] = net.AddPeer(1);
		net.SetLinkBothWays(senders[flow], receiver, rule);
		if (!net.Connect(senders[flow], receiver))
		{
			state.SkipWithError("handshake failed");
			return;
		}
	}
	const auto address = net.AddressOf(receiver);
	rule.lossPercent = static_cast<int>(state.range(1));
	for (size_t flow = 0; flow < flows; ++flow)
		net.SetLink(receiver, senders[flow], rule);
	rule.bottleneck = &path;
	for (size_t flow = 0; flow < flows; ++flow)
		net.SetLink(senders[flow], receiver, rule);
	std::array<uint32_t, 2> offered{}, delivered{};
	std::array<char, blockBytes> block{};
	block[0] = static_cast<char>(ID_USER_PACKET_ENUM);
	auto collect = [&]
	{
		for (const auto &m : net.Inbox(receiver))
		{
			if (m.id != ID_USER_PACKET_ENUM)
				continue;
			if (m.data.size() != blockBytes)
				return false;
			const size_t flow = m.data[1];
			uint32_t sequence;
			std::memcpy(&sequence, m.data.data() + 2, sizeof(sequence));
			if (flow >= flows || m.systemAddress != net.AddressOf(senders[flow]) || sequence != delivered[flow])
				return false;
			++delivered[flow];
		}
		net.Inbox(receiver).clear();
		for (size_t flow = 0; flow < flows; ++flow)
			net.Inbox(senders[flow]).clear();
		return true;
	};
	for (auto _ : state)
		for (unsigned ms = 0; ms < 60000; ++ms)
		{
			for (size_t flow = 0; flow < flows; ++flow)
				while (offered[flow] - delivered[flow] < 16)
				{
					block[1] = static_cast<char>(flow);
					std::memcpy(block.data() + 2, &offered[flow], sizeof(uint32_t));
					if (!senders[flow]->Send(block.data(), static_cast<int>(block.size()), Priority::High, Reliability::ReliableOrdered, 0, address, false))
					{
						state.SkipWithError("send failed");
						return;
					}
					++offered[flow];
				}
			net.Step(1);
			if (!collect())
			{
				state.SkipWithError("shared flow sequence mismatch");
				return;
			}
		}
	double sum = 0, squares = 0;
	for (size_t flow = 0; flow < flows; ++flow)
	{
		const double goodput = static_cast<double>(delivered[flow]) * blockBytes / 60;
		state.counters["flow" + std::to_string(flow) + "_B_per_s"] = goodput;
		sum += goodput;
		squares += goodput * goodput;
	}
	if (!squares)
	{
		state.SkipWithError("no deliveries by cutoff");
		return;
	}
	state.counters["jain_fairness"] = sum * sum / (flows * squares);
	state.counters["aggregate_B_per_s"] = sum;
	state.counters["path_random_drops"] = static_cast<double>(path.randomDrops);
	state.counters["path_tail_drops"] = static_cast<double>(path.drops);
	state.counters["path_peak_queue_B"] = path.peakBytes;
	unsigned drain = 0;
	while (delivered != offered && drain < 60000)
	{
		++drain;
		net.Step(1);
		if (!collect())
		{
			state.SkipWithError("shared flow drain sequence mismatch");
			return;
		}
	}
	if (delivered != offered)
	{
		state.SkipWithError("shared flows failed to drain");
		return;
	}
	state.counters["drain_ms"] = drain;
}
void SharedCases(benchmark::Benchmark *bench)
{
	for (int flows : {1, 2})
	{
		bench->Args({flows, 0, 1});
		for (int seed = 1; seed <= 5; ++seed)
			bench->Args({flows, 2, seed});
	}
}
BENCHMARK(RakPeer_SharedBottleneck)->Apply(SharedCases)->ArgNames({"flows", "loss_pct", "seed"})->Iterations(1);
}
