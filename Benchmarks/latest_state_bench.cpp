// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#include <benchmark/benchmark.h>

#include "SimulatedNetwork.h"
#include "mafianet/internal_packet.h"
#include "mafianet/state_update_queue.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <random>

using namespace MafiaNet;
namespace
{
constexpr MessageID stateId = ID_USER_PACKET_ENUM, eventId = stateId + 1, bulkId = stateId + 2;
constexpr int objects = 12, recordBytes = 240, duration = 60000;

template <typename Visit>
bool VisitRecords(const unsigned char *data, size_t bytes, Visit visit)
{
	while (bytes)
	{
		const unsigned char *record;
		size_t size;
		if (!StateUpdateQueue::ReadRecord(data, bytes, record, size) || size != recordBytes || record[0] >= objects)
			return false;
		int generated;
		std::memcpy(&generated, record + 1, sizeof(generated));
		visit(record[0], generated);
	}
	return true;
}

struct Observer : PluginInterface2
{
	int now = 0;
	int promoted = 0;
	std::vector<int> queueTimes;
	bool valid = true;
	bool UsesReliabilityLayer() const override { return true; }
	void OnInternalPacket(InternalPacket *p, unsigned, SystemAddress, TimeMS, int sending) override
	{
		if (!sending || p->timesSent > 1 || (p->splitPacketCount && p->splitPacketIndex != 0))
			return;
		const auto *data = p->splitPacketCount ? p->refCountedData->sharedDataBlock : p->data;
		const auto bytes = BITS_TO_BYTES(p->splitPacketCount ? p->splitOriginalBitLength : p->dataBitLength);
		if (bytes && data[0] == stateId)
		{
			promoted += p->reliability == Reliability::Reliable;
			valid &= VisitRecords(data + 1, bytes - 1, [&](int, int generated)
			                      { queueTimes.push_back(now - generated); });
		}
	}
};

int Percentile95(std::vector<int> &samples)
{
	const auto pos = (samples.size() * 95 + 99) / 100 - 1;
	std::nth_element(samples.begin(), samples.begin() + pos, samples.end());
	return samples[pos];
}

// Both policies generate the same object values, reliable events and bulk load.
// Sample receiver age every tick, including ticks where nothing arrived.
void LatestState(benchmark::State &state)
{
	for (auto _ : state)
	{
		const int policy = static_cast<int>(state.range(0));
		const bool packed = policy == 1 || policy == 3;
		const bool lossless = policy >= 2;
		const auto seed = static_cast<unsigned>(state.range(1));
		Observer observer;
		SimulatedNetwork net(seed);
		auto *server = net.AddPeer(1, 1);
		auto *client = net.AddPeer(1);
		if (!net.Connect(client, server))
		{
			state.SkipWithError("connection failed");
			return;
		}
		server->AttachPlugin(&observer);
		const auto target = net.AddressOf(client);
		SimulatedNetwork::LinkRule link;
		link.lossPercent = 2;
		link.delayMs = 50;
		link.reorderPerThousand = 5;
		link.reorderDelayMs = 25;
		net.SetLinkBothWays(server, client, link);
		server->SetPerConnectionOutgoingBandwidthLimit(256 * 1024 * 8);
		const auto budget = StateUpdateQueue::PayloadBudget(server->GetMTUSize(target));
		std::mt19937 rng(seed);
		std::bernoulli_distribution reliableEvent(0.30);
		std::vector<unsigned char> block(512 * 1024, 0);
		block[0] = bulkId;
		std::vector<unsigned char> event(objects * recordBytes, 0);
		event[0] = eventId;
		StateUpdateQueue queue(duration * objects, lossless ? StateUpdateQueue::Delivery::ReliableOrdered : StateUpdateQueue::Delivery::LatestOnly);
		std::array<int, objects> latest{};
		std::vector<int> ages;
		std::array<std::vector<int>, objects> expected;
		std::array<size_t, objects> received{};
		int superseded = 0;
		int bulkSent = 0, bulkReceived = 0, eventsSent = 0, eventsReceived = 0, offered = 0, delivered = 0;
		auto send = [&](const auto &bytes, Priority priority, Reliability reliability, char channel)
		{
			return server->Send(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), priority, reliability, channel, target, false) != 0;
		};
		int drainedAt = 0;
		for (int ms = 0; ms < duration + 180000 && observer.valid; ++ms)
		{
			if (ms < duration && bulkSent - bulkReceived < 2)
			{
				observer.valid &= send(block, Priority::Low, Reliability::ReliableOrdered, 2);
				++bulkSent;
			}
			const bool tick = ms < duration && (ms == 0 || ms * 30 / 1000 != (ms - 1) * 30 / 1000);
			if (tick && reliableEvent(rng))
			{
				std::memcpy(event.data() + 1, &eventsSent, sizeof(eventsSent));
				observer.valid &= send(event, Priority::High, Reliability::ReliableOrdered, 1);
				++eventsSent;
			}
			else if (tick)
			{
				std::vector<unsigned char> batch{stateId};
				for (int object = 0; object < objects; ++object)
				{
					std::array<unsigned char, recordBytes> record{};
					record[0] = static_cast<unsigned char>(object);
					std::memcpy(record.data() + 1, &ms, sizeof(ms));
					if (packed)
					{
						const auto previousSize = queue.Size();
						observer.valid &= queue.Put(object, record.data(), record.size(), budget);
						superseded += previousSize == queue.Size();
					}
					else
					{
						batch.push_back(0);
						batch.push_back(recordBytes);
						batch.insert(batch.end(), record.begin(), record.end());
					}
					expected[object].push_back(ms);
					++offered;
				}
				if (!packed)
					observer.valid &= send(batch, Priority::High, lossless ? Reliability::ReliableOrdered : Reliability::Unreliable, 0);
			}
			if (packed)
			{
				uint64_t key = 0;
				const auto result = queue.SendOne(*server, target, stateId, key);
				observer.valid &= result != StateUpdateQueue::Result::Disconnected && result != StateUpdateQueue::Result::RecordTooLarge;
			}
			observer.now = ms + 1;
			net.Step(1);
			for (const auto &m : net.Inbox(client))
			{
				if (m.id == bulkId)
					++bulkReceived;
				if (m.id == eventId)
				{
					observer.valid &= m.data.size() == event.size() && std::memcmp(m.data.data() + 1, &eventsReceived, sizeof(eventsReceived)) == 0;
					++eventsReceived;
				}
				if (m.id == stateId)
					observer.valid &= VisitRecords(m.data.data() + 1, m.data.size() - 1, [&](int object, int generated)
					                               {
						if (lossless)
						{
							const auto index = received[object]++;
							observer.valid &= index < expected[object].size() && expected[object][index] == generated;
						}
						latest[object] = std::max(latest[object], generated);
						++delivered; });
			}
			net.Inbox(client).clear();
			if (tick)
				for (const int last : latest)
					ages.push_back(ms + 1 - last);
			if (ms + 1 == duration)
			{
				if (observer.queueTimes.empty())
				{
					state.SkipWithError("no state transmitted");
					return;
				}
				state.counters["state_queue_p95_ms"] = Percentile95(observer.queueTimes);
				state.counters["state_messages_promoted"] = observer.promoted;
				state.counters["state_received"] = delivered;
				state.counters["events_received"] = eventsReceived;
				state.counters["bulk_B_per_s"] = bulkReceived * 512.0 * 1024 / (duration / 1000);
			}
			if (ms + 1 >= duration && eventsReceived == eventsSent && bulkReceived == bulkSent && (!lossless || delivered == offered))
			{
				drainedAt = ms + 1;
				break;
			}
		}
		if (!observer.valid || !drainedAt || observer.queueTimes.empty() || ages.empty())
		{
			state.SkipWithError("failed to deliver or trace a state record");
			return;
		}
		state.counters["state_age_p95_ms"] = Percentile95(ages);
		state.counters["state_records_superseded"] = superseded;
		state.counters["state_offered"] = offered;
		state.counters["events_offered"] = eventsSent;
		state.counters["events_after_drain"] = eventsReceived;
		state.counters["state_after_drain"] = delivered;
		state.counters["drain_ms"] = drainedAt - duration;
	}
}
BENCHMARK(LatestState)->ArgsProduct({{0, 1, 2, 3}, {1, 2, 3, 4, 5}})->ArgNames({"policy", "seed"})->Iterations(1);
}
