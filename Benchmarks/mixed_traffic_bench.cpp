/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */
#include <benchmark/benchmark.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>

#include "SimulatedNetwork.h"
#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;

namespace
{
constexpr int gameplay = 0;
constexpr int urgent = 1;
constexpr int bulk = 2;
constexpr int durationMs = 60000;
constexpr size_t prefixBytes = 1 + sizeof(uint32_t);

// The first five bytes of each application message hold its class and index.
// Sender and receiver observers share this table on the single-threaded simulator.
struct MessageTrace
{
	int queued = 0;
	int firstSent = -1;
	int firstResent = -1;
	int complete = -1;
	int delivered = -1;
	int resends = 0;
	bool requestedUnreliable = false;
	bool promotedToReliable = false;
	std::vector<bool> receivedFragments;
	unsigned received = 0;
};

struct Traces
{
	std::array<std::vector<MessageTrace>, 3> messages;
	std::map<SplitPacketIdType, std::pair<int, uint32_t>> splitMessages;
	int now = 0;
	int bulkDelivered = 0;
	bool valid = true;

	bool Identify(const unsigned char *data, size_t bytes, int &lane, uint32_t &index) const
	{
		if (bytes < prefixBytes || data[0] < ID_USER_PACKET_ENUM)
			return false;
		lane = data[0] - ID_USER_PACKET_ENUM;
		if (lane > bulk)
			return false;
		std::memcpy(&index, data + 1, sizeof(index));
		return index < messages[lane].size();
	}

	void Deliver(const SimulatedNetwork::Message &message)
	{
		int lane;
		uint32_t index;
		if (message.id < ID_USER_PACKET_ENUM)
			return;
		if (!Identify(message.data.data(), message.data.size(), lane, index))
		{
			valid = false;
			return;
		}
		auto &trace = messages[lane][index];
		if (trace.complete < 0 || trace.delivered >= 0)
			valid = false;
		trace.delivered = now;
		if (lane == bulk)
			++bulkDelivered;
	}
};

class FragmentObserver : public PluginInterface2
{
public:
	FragmentObserver(Traces &traces, bool sender)
	    : traces(traces), sender(sender) {}
	bool UsesReliabilityLayer() const override { return true; }

	void OnInternalPacket(InternalPacket *packet, unsigned, SystemAddress, TimeMS, int isSend) override
	{
		if (sender != static_cast<bool>(isSend))
			return;
		int lane;
		uint32_t index;
		const bool split = packet->splitPacketCount != 0;
		if (split && !sender)
		{
			const auto found = traces.splitMessages.find(packet->splitPacketId);
			if (found == traces.splitMessages.end())
				return;
			lane = found->second.first;
			index = found->second.second;
		}
		else
		{
			// On send, all fragments share the allocation holding the original message.
			const auto *data = split ? packet->refCountedData->sharedDataBlock : packet->data;
			const size_t bytes = BITS_TO_BYTES(split ? packet->splitOriginalBitLength : packet->dataBitLength);
			if (!traces.Identify(data, bytes, lane, index))
				return;
			if (split)
				traces.splitMessages[packet->splitPacketId] = {lane, index};
		}
		auto &trace = traces.messages[lane][index];
		if (sender)
		{
			if (trace.firstSent < 0)
			{
				trace.firstSent = traces.now;
				trace.promotedToReliable = trace.requestedUnreliable && packet->reliability == Reliability::Reliable;
			}
			if (packet->timesSent > 1)
			{
				if (trace.firstResent < 0)
					trace.firstResent = traces.now;
				++trace.resends;
			}
		}
		else if (trace.complete < 0)
		{
			if (split)
			{
				if (trace.receivedFragments.empty())
					trace.receivedFragments.resize(packet->splitPacketCount);
				if (packet->splitPacketIndex >= trace.receivedFragments.size())
				{
					traces.valid = false;
					return;
				}
				if (!trace.receivedFragments[packet->splitPacketIndex])
				{
					trace.receivedFragments[packet->splitPacketIndex] = true;
					++trace.received;
				}
			}
			if (!split || trace.received == packet->splitPacketCount)
				trace.complete = traces.now;
		}
	}

private:
	Traces &traces;
	bool sender;
};

void Report(benchmark::State &state, const std::string &prefix, const std::vector<MessageTrace> &messages)
{
	unsigned started = 0, retried = 0, complete = 0, delivered = 0, resends = 0;
	unsigned unreliable = 0, promoted = 0;
	double queue = 0, retry = 0, assembly = 0, delivery = 0, total = 0;
	for (const auto &m : messages)
	{
		if (m.firstSent >= 0)
		{
			++started;
			unreliable += m.requestedUnreliable;
			promoted += m.promotedToReliable;
			queue += m.firstSent - m.queued;
		}
		if (m.firstResent >= 0)
		{
			++retried;
			retry += m.firstResent - m.firstSent;
			resends += m.resends;
		}
		if (m.complete >= 0)
		{
			++complete;
			assembly += m.complete - m.firstSent;
		}
		if (m.delivered >= 0)
		{
			++delivered;
			delivery += m.delivered - m.complete;
			total += m.delivered - m.queued;
		}
	}
	state.counters[prefix + "_offered"] = static_cast<double>(messages.size());
	state.counters[prefix + "_never_sent"] = static_cast<double>(messages.size() - started);
	state.counters[prefix + "_started_incomplete"] = started - complete;
	state.counters[prefix + "_complete_undelivered"] = complete - delivered;
	state.counters[prefix + "_delivered"] = delivered;
	state.counters[prefix + "_retried_messages"] = retried;
	state.counters[prefix + "_fragment_resends"] = resends;
	if (unreliable)
	{
		state.counters[prefix + "_unreliable_started"] = unreliable;
		state.counters[prefix + "_upgraded_to_reliable"] = promoted;
	}
	if (started)
		state.counters[prefix + "_queue_ms"] = queue / started;
	if (retried)
		state.counters[prefix + "_first_retry_ms"] = retry / retried;
	if (complete)
		state.counters[prefix + "_send_to_complete_ms"] = assembly / complete;
	if (delivered)
	{
		state.counters[prefix + "_complete_to_app_ms"] = delivery / delivered;
		state.counters[prefix + "_latency_ms"] = total / delivered;
	}
}

void RakPeer_MixedTraffic(benchmark::State &state)
{
	const auto loss = static_cast<int>(state.range(0));
	const auto blockBytes = static_cast<int>(state.range(1)) * 1024;
	const auto seed = static_cast<unsigned>(state.range(2));
	Traces traces;
	FragmentObserver sender(traces, true), receiver(traces, false);
	SimulatedNetwork net(seed);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	// Run the same registered cases (and identical seeds) under either policy.
	const char *policy = std::getenv("MAFIANET_BENCH_SCHEDULER");
	OutgoingSchedulingPolicy scheduling;
	if (policy && std::strcmp(policy, "legacy") != 0)
	{
		scheduling.mode = OutgoingSchedulingMode::WeightedBytes;
		if (std::strcmp(policy, "7:2:1") == 0)
			scheduling.high = 7;
		else if (std::strcmp(policy, "3:2:1") != 0)
		{
			state.SkipWithError("MAFIANET_BENCH_SCHEDULER must be legacy, 3:2:1 or 7:2:1");
			return;
		}
	}
	server->SetOutgoingSchedulingPolicy(scheduling);
	const bool weighted = scheduling.mode == OutgoingSchedulingMode::WeightedBytes;
	state.counters["scheduler_high_weight"] = weighted ? scheduling.high : 0;
	state.counters["scheduler_medium_weight"] = weighted ? scheduling.medium : 0;
	state.counters["scheduler_low_weight"] = weighted ? scheduling.low : 0;
	if (!net.Connect(client, server))
	{
		state.SkipWithError("handshake failed");
		return;
	}
	const auto serverAddress = net.AddressOf(server);
	const auto clientAddress = net.AddressOf(client);
	server->AttachPlugin(&sender);
	client->AttachPlugin(&receiver);
	SimulatedNetwork::LinkRule link;
	link.delayMs = 50;
	link.lossPercent = loss;
	link.reorderPerThousand = 5;
	link.reorderDelayMs = 25;
	net.SetLinkBothWays(server, client, link);
	server->SetPerConnectionOutgoingBandwidthLimit(256 * 1024 * 8);

	std::mt19937 random(seed);
	auto draw = [&](int min, int max)
	{ return std::uniform_int_distribution<int>(min, max)(random); };
	std::vector<unsigned char> payload(blockBytes);
	auto send = [&](bool downstream, int lane, int length, Priority priority, Reliability reliability)
	{
		uint32_t index = 0;
		if (downstream)
		{
			auto &messages = traces.messages[lane];
			index = static_cast<uint32_t>(messages.size());
			auto &trace = messages.emplace_back();
			trace.queued = traces.now;
			trace.requestedUnreliable = reliability == Reliability::Unreliable;
		}
		payload[0] = static_cast<unsigned char>(ID_USER_PACKET_ENUM + lane);
		std::memcpy(payload.data() + 1, &index, sizeof(index));
		auto *peer = downstream ? server : client;
		if (!peer->Send(reinterpret_cast<const char *>(payload.data()), length, priority, reliability, static_cast<char>(lane), downstream ? clientAddress : serverAddress, false))
			traces.valid = false;
	};

	for (auto _ : state)
	{
		int nextUrgent = 0;
		for (int ms = 0; ms < durationMs && traces.valid; ++ms)
		{
			traces.now = ms;
			// Keep one MiB of bulk data outstanding in both block-size variants.
			if (static_cast<int>(traces.messages[bulk].size()) - traces.bulkDelivered < 1024 * 1024 / blockBytes)
				send(true, bulk, blockBytes, Priority::Low, Reliability::ReliableOrdered);
			if (ms >= nextUrgent)
			{
				send(true, urgent, draw(100, 500), Priority::Immediate, Reliability::ReliableOrdered);
				nextUrgent = ms + draw(500, 1500);
			}
			if (ms == 0 || ms * 30 / 1000 != (ms - 1) * 30 / 1000)
			{
				for (const bool downstream : {true, false})
				{
					const int size = downstream ? draw(1000, 5000) : draw(100, 2000);
					const auto reliability = draw(0, 100) < 30 ? Reliability::ReliableOrdered : Reliability::Unreliable;
					send(downstream, gameplay, size, Priority::High, reliability);
				}
			}
			traces.now = ms + 1;
			net.Step(1);
			for (const auto &m : net.Inbox(client))
				traces.Deliver(m);
			net.Inbox(client).clear();
			net.Inbox(server).clear();
		}
	}
	if (!traces.valid || client->NumberOfConnections() != 1 || server->NumberOfConnections() != 1)
	{
		state.SkipWithError("send, trace correlation or connection failed");
		return;
	}
	Report(state, "game", traces.messages[gameplay]);
	Report(state, "urgent", traces.messages[urgent]);
	Report(state, "bulk", traces.messages[bulk]);
	state.counters["bulk_B_per_sim_s"] = static_cast<double>(traces.bulkDelivered) * blockBytes * 1000 / durationMs;
}
BENCHMARK(RakPeer_MixedTraffic)->ArgsProduct({{2, 0}, {512, 16}, {1, 2, 3, 4, 5}})->ArgNames({"loss_pct", "block_KiB", "seed"})->Iterations(1);
}
