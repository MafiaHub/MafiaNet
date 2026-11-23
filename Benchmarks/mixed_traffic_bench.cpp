/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <string>

#include "SimulatedNetwork.h"
#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;

namespace
{
void RunMixedTraffic(benchmark::State &state, bool allReliable, bool finitePath = false);

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
	unsigned bytes = 0;
	unsigned firstSendBytes = 0;
	unsigned resendBytes = 0;
	bool requestedUnreliable = false;
	bool promotedToReliable = false;
	bool fragmented = false;
	std::vector<bool> receivedFragments;
	unsigned received = 0;
};

struct QueueDepth
{
	uint32_t bytes = 0;
	uint64_t sumBytes = 0;
	int oldestMs = 0;
};

struct Traces
{
	std::array<std::vector<MessageTrace>, 3> messages;
	std::map<SplitPacketIdType, std::pair<int, uint32_t>> splitMessages;
	std::array<QueueDepth, 3> depth{};
	int now = 0;
	int bulkDelivered = 0;
	uint64_t deliveredBytes = 0;
	bool valid = true;

	uint32_t Enqueue(int lane, unsigned bytes, Reliability reliability)
	{
		auto &trace = messages[lane].emplace_back();
		trace.queued = now;
		trace.bytes = bytes;
		trace.requestedUnreliable = reliability == Reliability::Unreliable;
		return static_cast<uint32_t>(messages[lane].size() - 1);
	}

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

	bool IdentifyFragment(const InternalPacket &packet, int &lane, uint32_t &index) const
	{
		// The sender's fragments share the original application payload.
		const bool split = packet.splitPacketCount != 0;
		const auto *data = split ? packet.refCountedData->sharedDataBlock : packet.data;
		const auto bytes = BITS_TO_BYTES(split ? packet.splitOriginalBitLength : packet.dataBitLength);
		return Identify(data, bytes, lane, index);
	}

	void SampleQueued(const InternalPacket &packet)
	{
		int lane;
		uint32_t index;
		if (IdentifyFragment(packet, lane, index))
		{
			auto &queue = depth[lane];
			queue.bytes += BITS_TO_BYTES(packet.dataBitLength);
			queue.oldestMs = std::max(queue.oldestMs, now - messages[lane][index].queued);
		}
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
		deliveredBytes += trace.bytes;
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
			if (!traces.IdentifyFragment(*packet, lane, index))
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
				trace.fragmented = split;
				trace.promotedToReliable = trace.requestedUnreliable && packet->reliability == Reliability::Reliable;
			}
			const unsigned bytes = BITS_TO_BYTES(packet->dataBitLength);
			if (packet->timesSent == 1)
			{
				if (bytes > trace.bytes - trace.firstSendBytes)
				{
					traces.valid = false;
					return;
				}
				trace.firstSendBytes += bytes;
			}
			if (packet->timesSent > 1)
			{
				if (trace.firstResent < 0)
					trace.firstResent = traces.now;
				++trace.resends;
				trace.resendBytes += bytes;
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

bool ConsumeOrdered(const SimulatedNetwork::Message &message, std::array<uint32_t, 3> &next)
{
	if (message.id < ID_USER_PACKET_ENUM)
		return true;
	const unsigned lane = message.id - ID_USER_PACKET_ENUM;
	if (lane >= next.size() || message.data.size() < prefixBytes)
		return false;
	uint32_t index;
	std::memcpy(&index, message.data.data() + 1, sizeof(index));
	return index == next[lane]++;
}

template <class T>
T Percentile(std::vector<T> &times, size_t percentile)
{
	const auto nth = times.begin() + (percentile * times.size() + 99) / 100 - 1;
	std::nth_element(times.begin(), nth, times.end());
	return *nth;
}

void Report(benchmark::State &state, const std::string &prefix, const std::vector<MessageTrace> &messages, const QueueDepth &depth, unsigned samples)
{
	unsigned started = 0, retried = 0, complete = 0, delivered = 0, resends = 0;
	unsigned unreliable = 0, promoted = 0;
	uint64_t offeredBytes = 0, firstSendBytes = 0, deliveredBytes = 0, resendBytes = 0;
	std::vector<int> firstSendTimes, deliveryTimes;
	double queue = 0, retry = 0, assembly = 0, delivery = 0, total = 0;
	for (const auto &m : messages)
	{
		offeredBytes += m.bytes;
		firstSendBytes += m.firstSendBytes;
		resendBytes += m.resendBytes;
		if (m.firstSent >= 0)
		{
			++started;
			unreliable += m.requestedUnreliable;
			promoted += m.promotedToReliable;
			firstSendTimes.push_back(m.firstSent - m.queued);
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
			deliveredBytes += m.bytes;
			delivery += m.delivered - m.complete;
			deliveryTimes.push_back(m.delivered - m.queued);
			total += m.delivered - m.queued;
		}
	}
	state.counters[prefix + "_offered_B"] = static_cast<double>(offeredBytes);
	state.counters[prefix + "_first_send_B"] = static_cast<double>(firstSendBytes);
	state.counters[prefix + "_resend_B"] = static_cast<double>(resendBytes);
	state.counters[prefix + "_delivered_B"] = static_cast<double>(deliveredBytes);
	state.counters[prefix + "_end_queue_B"] = depth.bytes;
	state.counters[prefix + "_unsent_outside_queue_B"] = static_cast<double>(offeredBytes - firstSendBytes) - depth.bytes;
	state.counters[prefix + "_mean_queue_B"] = static_cast<double>(depth.sumBytes) / samples;
	state.counters[prefix + "_oldest_queued_ms"] = depth.oldestMs;
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
	{
		state.counters[prefix + "_queue_ms"] = queue / started;
		state.counters[prefix + "_queue_p95_ms"] = Percentile(firstSendTimes, 95);
	}
	if (retried)
		state.counters[prefix + "_first_retry_ms"] = retry / retried;
	if (complete)
		state.counters[prefix + "_send_to_complete_ms"] = assembly / complete;
	if (delivered)
	{
		state.counters[prefix + "_complete_to_app_ms"] = delivery / delivered;
		state.counters[prefix + "_latency_ms"] = total / delivered;
		state.counters[prefix + "_latency_p95_ms"] = Percentile(deliveryTimes, 95);
		state.counters[prefix + "_latency_p99_ms"] = Percentile(deliveryTimes, 99);
	}
}

void ConfigureGameplayLink(SimulatedNetwork &net, RakPeer *server, RakPeer *client, int loss)
{
	SimulatedNetwork::LinkRule link;
	link.delayMs = 50;
	link.lossPercent = loss;
	link.reorderPerThousand = 5;
	link.reorderDelayMs = 25;
	net.SetLinkBothWays(server, client, link);
	server->SetPerConnectionOutgoingBandwidthLimit(256 * 1024 * 8);
}

void RakPeer_MixedTraffic(benchmark::State &state)
{
	RunMixedTraffic(state, false);
}

void RunMixedTraffic(benchmark::State &state, bool allReliable, bool finitePath)
{
	const auto loss = static_cast<int>(state.range(0));
	const auto blockBytes = static_cast<int>(state.range(1)) * 1024;
	const auto seed = static_cast<unsigned>(state.range(2));
	Traces traces;
	FragmentObserver sender(traces, true), receiver(traces, false);
	std::optional<SimulatedBottleneck> pathStorage; // outlive every link that refers to the FIFO
	uint64_t wireBytes = 0;
	SimulatedNetwork net(seed);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	if (finitePath)
	{
		SimulatedNetwork::LinkRule propagation;
		propagation.delayMs = 50;
		net.SetLinkBothWays(server, client, propagation);
	}
	if (!net.Connect(client, server))
	{
		state.SkipWithError("handshake failed");
		return;
	}
	const auto serverAddress = net.AddressOf(server);
	const auto clientAddress = net.AddressOf(client);
	server->AttachPlugin(&sender);
	client->AttachPlugin(&receiver);
	ConfigureGameplayLink(net, server, client, loss);

	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *, int size)
	           {
		if (from == serverAddress)
			wireBytes += size; });
	const auto epoch = net.Now();
	std::vector<SimulatedBottleneck::Rate> rates{{0, 256 * 1024}};
	if (finitePath && state.range(3) == 1)
	{
		rates.push_back({epoch + 20000000, 64 * 1024});
		rates.push_back({epoch + 40000000, 256 * 1024});
	}
	auto &bottleneck = pathStorage.emplace(51200, std::move(rates));
	if (finitePath)
	{
		SimulatedNetwork::LinkRule link;
		link.delayMs = 50;
		link.lossPercent = loss;
		link.reorderPerThousand = static_cast<uint32_t>(state.range(4));
		link.reorderDelayMs = 25;
		net.SetLink(client, server, link);
		link.bottleneck = &bottleneck;
		net.SetLink(server, client, link);
		server->SetPerConnectionOutgoingBandwidthLimit(0);
	}

	std::mt19937 random(seed);
	auto draw = [&](int min, int max)
	{ return std::uniform_int_distribution<int>(min, max)(random); };
	std::vector<unsigned char> payload(std::max(5000, blockBytes));
	uint32_t upstreamOffered = 0;
	std::array<uint32_t, 3> downstreamReceived{}, upstreamReceived{};
	auto collect = [&]
	{
		for (const auto &m : net.Inbox(client))
		{
			traces.Deliver(m);
			if (allReliable && !ConsumeOrdered(m, downstreamReceived))
				traces.valid = false;
		}
		if (allReliable)
			for (const auto &m : net.Inbox(server))
				if (!ConsumeOrdered(m, upstreamReceived))
					traces.valid = false;
		net.Inbox(client).clear();
		net.Inbox(server).clear();
	};
	auto send = [&](bool downstream, int lane, int length, Priority priority, Reliability reliability)
	{
		const uint32_t index = downstream ? traces.Enqueue(lane, static_cast<unsigned>(length), reliability) : 0;
		payload[0] = static_cast<unsigned char>(ID_USER_PACKET_ENUM + lane);
		const uint32_t sequence = downstream ? index : upstreamOffered++;
		std::memcpy(payload.data() + 1, &sequence, sizeof(sequence));
		auto *peer = downstream ? server : client;
		if (!peer->Send(reinterpret_cast<const char *>(payload.data()), length, priority, reliability, static_cast<char>(lane), downstream ? clientAddress : serverAddress, false))
			traces.valid = false;
	};

	unsigned depthSamples = 0;
	uint64_t lastDelivered = 0, lastTransmitted = 0, lastDrops = 0, depthByteMs = 0;
	size_t firstWait = 0;
	const std::function<void(const InternalPacket &)> noVisit;
	std::function<void(const InternalPacket &)> sampleQueued = [&](const InternalPacket &packet)
	{ traces.SampleQueued(packet); };
#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
	uint64_t windowSum = 0, inFlightSum = 0, availableSum = 0;
	unsigned queuedMs = 0, windowOnlyMs = 0, capOnlyMs = 0, bothMs = 0;
#endif
	for (auto _ : state)
	{
		int nextUrgent = 0;
		for (int ms = 0; ms < durationMs && traces.valid; ++ms)
		{
			traces.now = ms;
			// Keep one MiB of bulk data outstanding in both block-size variants.
			if (blockBytes && static_cast<int>(traces.messages[bulk].size()) - traces.bulkDelivered < 1024 * 1024 / blockBytes)
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
					send(downstream, gameplay, size, Priority::High, allReliable ? Reliability::ReliableOrdered : reliability);
				}
			}
			traces.now = ms + 1;
			net.Step(1);
			// Periodically visit the live send heap. Unreliable expiry and partial
			// sends make offered minus first-sent bytes an inaccurate queue depth.
			const bool sampleDepth = traces.now % 100 == 0;
			if (sampleDepth)
				for (auto &d : traces.depth)
				{
					d.bytes = 0;
					d.oldestMs = 0;
				}
			SimulatedNetwork::SendBudget budget;
			if (!net.ReadSendBudget(server, clientAddress, budget, sampleDepth ? sampleQueued : noVisit))
			{
				traces.valid = false;
				break;
			}
			if (sampleDepth)
			{
				for (auto &d : traces.depth)
					d.sumBytes += d.bytes;
				++depthSamples;
			}
#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
			windowSum += budget.windowBytes;
			inFlightSum += budget.inFlightBytes;
			const uint32_t available = budget.windowBytes - std::min(budget.windowBytes, budget.inFlightBytes);
			availableSum += available;
			if (budget.queued)
			{
				++queuedMs;
				if (!available && budget.capBlocked)
					++bothMs;
				else if (!available)
					++windowOnlyMs;
				else if (budget.capBlocked)
					++capOnlyMs;
			}
#endif
			collect();
			if (finitePath)
			{
				const auto depth = bottleneck.QueuedBytes(net.Now());
				depthByteMs += depth;
				if (traces.now % 20000 == 0)
				{
					const auto prefix = "phase" + std::to_string(traces.now / 20000) + "_";
					const auto transmitted = bottleneck.admittedBytes - depth;
					state.counters[prefix + "wire_B_per_s"] = static_cast<double>(transmitted - lastTransmitted) / 20;
					state.counters[prefix + "delivered_B_per_s"] = static_cast<double>(traces.deliveredBytes - lastDelivered) / 20;
					state.counters[prefix + "drops"] = static_cast<double>(bottleneck.drops - lastDrops);
					state.counters[prefix + "mean_queue_B"] = static_cast<double>(depthByteMs) / 20000;
					std::vector<TimeUS> waits(bottleneck.waitUs.begin() + firstWait, bottleneck.waitUs.end());
					if (!waits.empty())
						state.counters[prefix + "queue_p95_ms"] = static_cast<double>(Percentile(waits, 95)) / 1000.0;
					firstWait = bottleneck.waitUs.size();
					lastTransmitted = transmitted;
					lastDelivered = traces.deliveredBytes;
					lastDrops = bottleneck.drops;
					depthByteMs = 0;
				}
			}
		}
	}
	if (!traces.valid || client->NumberOfConnections() != 1 || server->NumberOfConnections() != 1)
	{
		state.SkipWithError("send, trace correlation or connection failed");
		return;
	}
	net.SetTap({});
	SimulatedNetwork::SendBudget budget;
	if (!net.ReadSendBudget(server, clientAddress, budget))
	{
		state.SkipWithError("connection lost at cutoff");
		return;
	}
	state.counters["nak_retry_B"] = static_cast<double>(budget.nakRetryBytes);
	state.counters["rto_retry_B"] = static_cast<double>(budget.rtoRetryBytes);
	state.counters["nak_retry_window_cuts"] = budget.nakRetryWindowCuts;
	state.counters["rto_retry_window_cuts"] = budget.rtoRetryWindowCuts;
	if (finitePath)
	{
		state.counters["path_random_drops"] = static_cast<double>(bottleneck.randomDrops);
		state.counters["path_tail_drops"] = static_cast<double>(bottleneck.drops);
		state.counters["path_tail_drop_B"] = static_cast<double>(bottleneck.droppedBytes);
		state.counters["path_peak_queue_B"] = bottleneck.peakBytes;
		if (!bottleneck.waitUs.empty())
		{
			state.counters["path_mean_queue_ms"] = static_cast<double>(std::accumulate(bottleneck.waitUs.begin(), bottleneck.waitUs.end(), uint64_t{0})) / static_cast<double>(bottleneck.waitUs.size()) / 1000;
			state.counters["path_queue_p95_ms"] = static_cast<double>(Percentile(bottleneck.waitUs, 95)) / 1000.0;
		}
	}
#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
	state.counters["mean_cwnd_B"] = static_cast<double>(windowSum) / durationMs;
	state.counters["mean_inflight_B"] = static_cast<double>(inFlightSum) / durationMs;
	state.counters["mean_available_window_B"] = static_cast<double>(availableSum) / durationMs;
	state.counters["queued_ms"] = queuedMs;
	state.counters["queued_cwnd_only_ms"] = windowOnlyMs;
	state.counters["queued_cap_only_ms"] = capOnlyMs;
	state.counters["queued_both_limits_ms"] = bothMs;
	state.counters["queued_neither_limit_ms"] = queuedMs - windowOnlyMs - capOnlyMs - bothMs;
#endif
	state.counters["downstream_udp_payload_B"] = static_cast<double>(wireBytes);
	Report(state, "game", traces.messages[gameplay], traces.depth[gameplay], depthSamples);
	Report(state, "urgent", traces.messages[urgent], traces.depth[urgent], depthSamples);
	Report(state, "bulk", traces.messages[bulk], traces.depth[bulk], depthSamples);
	state.counters["bulk_B_per_sim_s"] = static_cast<double>(traces.bulkDelivered) * blockBytes * 1000 / durationMs;
	if (allReliable)
	{
		// Preserve the 60-second workload counters above. Stop offering data
		// and keep the same link impairments while both directions drain.
		auto drained = [&]
		{
			SimulatedNetwork::SendBudget down, up;
			if (!net.ReadSendBudget(server, clientAddress, down) || !net.ReadSendBudget(client, serverAddress, up))
				return false;
			for (size_t lane = 0; lane < downstreamReceived.size(); ++lane)
				if (downstreamReceived[lane] != traces.messages[lane].size())
					return false;
			return upstreamReceived[gameplay] == upstreamOffered && !up.queued && !down.queued && up.inFlightBytes == 0 && down.inFlightBytes == 0;
		};
		while (traces.valid && !drained() && traces.now < (finitePath ? 6 : 2) * durationMs)
		{
			++traces.now;
			net.Step(1);
			collect();
		}
		if (!traces.valid || !drained())
		{
			state.SkipWithError("ordered delivery or bounded drain failed");
			return;
		}
		state.counters["drain_ms"] = traces.now - durationMs;
		state.counters["game_after_drain"] = downstreamReceived[gameplay];
		state.counters["up_game_after_drain"] = upstreamReceived[gameplay];
		state.counters["urgent_after_drain"] = downstreamReceived[urgent];
		state.counters["bulk_after_drain"] = downstreamReceived[bulk];
	}
}
void RakPeer_MixedTrafficReliable(benchmark::State &state)
{
	RunMixedTraffic(state, true);
}
void RakPeer_FiniteBottleneck(benchmark::State &state)
{
	RunMixedTraffic(state, true, true);
}

void FiniteCases(benchmark::Benchmark *bench)
{
	for (int seed = 1; seed <= 5; ++seed)
		for (int block : {0, 512})
		{
			bench->Args({0, block, seed, 0, 0});  // steady, clean
			bench->Args({2, block, seed, 0, 0});  // independent loss only
			bench->Args({0, block, seed, 0, 50}); // reorder only
			bench->Args({0, block, seed, 1, 0});  // abrupt capacity drop and recovery
		}
}
BENCHMARK(RakPeer_FiniteBottleneck)->Apply(FiniteCases)->ArgNames({"loss_pct", "block_KiB", "seed", "capacity_step", "reorder_per_mille"})->Iterations(1);
BENCHMARK(RakPeer_MixedTrafficReliable)->ArgsProduct({{2, 0}, {512, 16, 0}, {1, 2, 3, 4, 5}})->ArgNames({"loss_pct", "block_KiB", "seed"})->Iterations(1);
BENCHMARK(RakPeer_MixedTraffic)->ArgsProduct({{2, 0}, {512, 16, 0}, {1, 2, 3, 4, 5}})->ArgNames({"loss_pct", "block_KiB", "seed"})->Iterations(1);

void RakPeer_FragmentationThreshold(benchmark::State &state)
{
	const int loss = static_cast<int>(state.range(0));
	const bool split = state.range(1) != 0;
	Traces traces;
	FragmentObserver sender(traces, true), receiver(traces, false);
	uint64_t wireBytes = 0;
	SimulatedNetwork net(static_cast<unsigned>(state.range(2)));
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	if (!net.Connect(client, server))
	{
		state.SkipWithError("handshake failed");
		return;
	}
	const auto address = net.AddressOf(client);
	const int limit = static_cast<int>(server->GetMaxUnfragmentedPayload(address, Reliability::Unreliable));
	if (limit < static_cast<int>(prefixBytes))
	{
		state.SkipWithError("missing payload budget");
		return;
	}
	ConfigureGameplayLink(net, server, client, loss);
	const auto source = net.AddressOf(server);
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *, int size)
	           {
		if (from == source)
			wireBytes += size; });
	SimulatedNetwork::SendBudget before{}, after{};
	if (!net.ReadSendBudget(server, address, before))
	{
		state.SkipWithError("missing initial send budget");
		return;
	}
	server->AttachPlugin(&sender);
	client->AttachPlugin(&receiver);
	const int bytes = limit + (split ? 1 : 0);
	// Both arms offer exactly 3 * limit * (limit + 1) application bytes over 60 seconds.
	const int messages = 3 * (split ? limit : limit + 1);
	std::vector<unsigned char> payload(bytes);
	payload[0] = ID_USER_PACKET_ENUM + gameplay;
	traces.messages[gameplay].reserve(messages);
	for (auto _ : state)
	{
		for (int ms = 0; ms < durationMs && traces.valid; ++ms)
		{
			traces.now = ms;
			const auto due = static_cast<size_t>(static_cast<int64_t>(ms + 1) * messages / durationMs);
			while (traces.messages[gameplay].size() < due)
			{
				const auto index = traces.Enqueue(gameplay, static_cast<unsigned>(bytes), Reliability::Unreliable);
				std::memcpy(payload.data() + 1, &index, sizeof(index));
				if (!server->Send(reinterpret_cast<const char *>(payload.data()), bytes, Priority::High, Reliability::Unreliable, 0, address, false))
					traces.valid = false;
			}
			traces.now = ms + 1;
			net.Step(1);
			for (const auto &message : net.Inbox(client))
				traces.Deliver(message);
			net.Inbox(client).clear();
		}
	}
	std::vector<int> latencies;
	int started = 0, promoted = 0;
	int64_t queueMs = 0;
	for (const auto &trace : traces.messages[gameplay])
	{
		if (trace.firstSent >= 0)
		{
			++started;
			promoted += trace.promotedToReliable;
			queueMs += trace.firstSent - trace.queued;
			if (trace.fragmented != split || trace.promotedToReliable != split)
				traces.valid = false;
		}
		if (trace.delivered >= 0)
			latencies.push_back(trace.delivered - trace.queued);
	}
	if (!traces.valid || traces.messages[gameplay].size() != static_cast<size_t>(messages) || started == 0 || latencies.empty() || server->NumberOfConnections() != 1 || client->NumberOfConnections() != 1)
	{
		state.SkipWithError("unexpected fragmentation, delivery, or connection state");
		return;
	}
	if (!net.ReadSendBudget(server, address, after))
	{
		state.SkipWithError("missing final send budget");
		return;
	}
	state.counters["nak_retry_B"] = static_cast<double>(after.nakRetryBytes - before.nakRetryBytes);
	state.counters["rto_retry_B"] = static_cast<double>(after.rtoRetryBytes - before.rtoRetryBytes);
	state.counters["nak_retry_window_cuts"] = after.nakRetryWindowCuts - before.nakRetryWindowCuts;
	state.counters["rto_retry_window_cuts"] = after.rtoRetryWindowCuts - before.rtoRetryWindowCuts;
	state.counters["downstream_udp_payload_B"] = static_cast<double>(wireBytes);
	state.counters["payload_B"] = bytes;
	state.counters["offered_B"] = static_cast<double>(messages) * bytes;
	state.counters["offered"] = messages;
	state.counters["started"] = started;
	state.counters["promoted_to_reliable"] = promoted;
	state.counters["delivered"] = static_cast<double>(latencies.size());
	state.counters["queue_mean_ms"] = static_cast<double>(queueMs) / started;
	state.counters["delivery_p95_ms"] = Percentile(latencies, 95);
}
BENCHMARK(RakPeer_FragmentationThreshold)->ArgsProduct({{0, 1, 2}, {0, 1}, {1, 2, 3, 4, 5}})->ArgNames({"loss_pct", "split", "seed"})->Iterations(1);
}
