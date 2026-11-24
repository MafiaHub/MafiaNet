// Copyright (c) 2026, MafiaHub. MIT license.
#include "FragmentSnapshotStudy.h"
#include "SimulatedNetwork.h"
#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace FragmentSnapshotStudy
{
using namespace MafiaNet;
namespace
{
using Bytes = std::vector<unsigned char>;
constexpr unsigned stateBytes = 1800;
constexpr unsigned fragmentTimeoutMs = 250;
constexpr unsigned char firstPart = ID_USER_PACKET_ENUM + 1, lastPart = ID_USER_PACKET_ENUM + 2;
constexpr unsigned headerBytes = 1 + sizeof(uint32_t);

unsigned char Value(unsigned sequence, unsigned offset)
{
	return static_cast<unsigned char>((sequence * 31u + offset * 17u + (offset >> 8)) % 251u);
}

Bytes Encode(unsigned sequence, unsigned offset, unsigned size, unsigned char id)
{
	Bytes data(headerBytes + size);
	data[0] = id;
	std::memcpy(data.data() + 1, &sequence, sizeof(uint32_t));
	for (unsigned i = 0; i < size; ++i)
		data[headerBytes + i] = Value(sequence, offset + i);
	return data;
}

unsigned Percentile(std::vector<unsigned> &samples, unsigned percent)
{
	if (samples.empty())
		return 0;
	auto nth = samples.begin() + (percent * samples.size() + 99) / 100 - 1;
	std::nth_element(samples.begin(), nth, samples.end());
	return *nth;
}

// Sample the first piece of each measured publication when it first leaves the sender.
class SendQueueProbe : public PluginInterface2
{
public:
	explicit SendQueueProbe(unsigned hz)
	    : hz(hz) {}
	bool UsesReliabilityLayer() const override { return true; }
	void OnInternalPacket(InternalPacket *packet, unsigned, SystemAddress, TimeMS, int sending) override
	{
		if (!sending || packet->timesSent != 1 || (packet->splitPacketCount && packet->splitPacketIndex != 0))
			return;
		if (BITS_TO_BYTES(packet->dataBitLength) < headerBytes || (packet->data[0] != firstPart && packet->data[0] != ID_USER_PACKET_ENUM))
			return;
		uint32_t sequence;
		std::memcpy(&sequence, packet->data + 1, sizeof(sequence));
		if (sequence > warmCount)
			delays.push_back(now - (sequence - 1) * 1000 / hz);
	}
	unsigned hz, now = 0, warmCount = 0;
	std::vector<unsigned> delays;
};

// Count unique native fragment payloads as they arrive. With the default split store,
// all fragment buffers coexist with the assembled output during its final copy.
class NativeAssembly : public PluginInterface2
{
public:
	bool UsesReliabilityLayer() const override { return true; }
	void OnInternalPacket(InternalPacket *p, unsigned, SystemAddress, TimeMS, int sending) override
	{
		if (sending || !p->splitPacketCount)
			return;
		if (p->splitPacketCount != 2 || p->splitPacketIndex > 1 || p->reliability != Reliability::Reliable)
		{
			valid = false;
			return;
		}
		auto &assembly = received[p->splitPacketId];
		auto &last = assembly.lastMessage[p->splitPacketIndex];
		const unsigned message = p->reliableMessageNumber.val;
		// Retries keep their 24-bit message number. A reused 16-bit split ID carries newer numbers.
		const unsigned distance = (message - last) & 0xffffffu;
		if (last != unseen && (distance == 0 || distance >= 0x800000u))
			return;
		last = message;
		auto &sizes = assembly.sizes;
		auto &part = sizes[p->splitPacketIndex];
		if (part)
		{
			valid = false;
			return;
		}
		part = BITS_TO_BYTES(p->dataBitLength);
		if (p->splitPacketIndex == 0)
			firstFragmentBytes = static_cast<unsigned>(part);
		held += part;
		if (sizes[0] && sizes[1])
		{
			const auto total = sizes[0] + sizes[1];
			valid = valid && total == stateBytes + headerBytes;
			peak = std::max(peak, held + total);
			held -= total;
			sizes = {};
		}
		else
		{
			peakPartial = std::max(peakPartial, held);
			peak = std::max(peak, held);
		}
	}
	unsigned firstFragmentBytes = 0;
	bool valid = true;
	size_t held = 0, peak = 0, peakPartial = 0;

private:
	static constexpr unsigned unseen = std::numeric_limits<unsigned>::max();
	struct Assembly
	{
		std::array<size_t, 2> sizes{};
		std::array<unsigned, 2> lastMessage{unseen, unseen};
	};
	std::map<SplitPacketIdType, Assembly> received;
};

bool SendSnapshot(RakPeer &server, SystemAddress address, Policy policy, unsigned sequence, unsigned splitAt)
{
	if (policy == Policy::Replaceable)
	{
		for (const auto id : {firstPart, lastPart})
		{
			const unsigned offset = id == firstPart ? 0 : splitAt;
			const unsigned size = id == firstPart ? splitAt : stateBytes - splitAt;
			const auto data = Encode(sequence, offset, size, id);
			if (server.SendDatagram(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), Priority::High, Reliability::Unreliable, 0, address).status != DatagramSendStatus::Queued)
				return false;
		}
		return true;
	}
	const auto data = Encode(sequence, 0, stateBytes, ID_USER_PACKET_ENUM);
	return server.Send(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), Priority::High, Reliability::Unreliable, 0, address, false) != 0;
}

// Discover the native fragment boundary at the workload RTT before adding loss or contention.
unsigned Calibrate(SimulatedNetwork &net, RakPeer &sender, RakPeer &client, NativeAssembly &native)
{
	client.AttachPlugin(&native);
	if (!SendSnapshot(sender, net.AddressOf(&client), Policy::Promoted, 0, 0) ||
	    !net.RunUntilReceived(&client, ID_USER_PACKET_ENUM, 1000) ||
	    native.firstFragmentBytes <= headerBytes || native.firstFragmentBytes >= stateBytes)
		return 0;
	net.Step(300);
	net.Inbox(&client).clear();
	return native.firstFragmentBytes - headerBytes;
}

class Receiver
{
public:
	Receiver(Result &result, unsigned splitAt)
	    : result(result), splitAt(splitAt) {}
	void BeginMeasurement(unsigned published)
	{
		warmPublications = published;
		result.applied = result.completed = 0;
		result.peakPartialBytes = result.peakAssemblyBytes = pending.size();
	}
	void Expire(unsigned ms)
	{
		if (!pending.empty() && ms - began >= fragmentTimeoutMs)
			Discard();
	}
	bool Receive(SimulatedNetwork::Message &message, unsigned ms, std::size_t otherStreamsHeld = 0)
	{
		if (message.id < ID_USER_PACKET_ENUM)
			return true;
		if (message.data.size() < headerBytes)
			return false;
		uint32_t sequence;
		std::memcpy(&sequence, message.data.data() + 1, sizeof(sequence));
		if (!sequence || sequence > result.publications)
			return false;
		if (message.id == ID_USER_PACKET_ENUM)
			return message.data.size() == stateBytes + headerBytes && Complete(sequence, message.data.data() + headerBytes, ms);
		if (message.id != firstPart && message.id != lastPart)
			return false;
		const unsigned size = message.id == firstPart ? splitAt : stateBytes - splitAt;
		if (message.data.size() != size + headerBytes)
			return false;
		if (sequence < newest || (sequence == newest && (pending.empty() || pending[0] == message.id)))
			return true;
		if (sequence > newest)
		{
			newest = sequence;
			began = ms;
			pending = std::move(message.data);
			result.remainingPartialBytes = pending.size();
			result.peakPartialBytes = std::max(result.peakPartialBytes, pending.size());
			result.peakAssemblyBytes = std::max(result.peakAssemblyBytes, otherStreamsHeld + pending.size());
			return true;
		}

		const auto &first = message.id == firstPart ? message.data : pending;
		const auto &last = message.id == lastPart ? message.data : pending;
		Bytes assembled(stateBytes);
		std::copy(first.begin() + headerBytes, first.end(), assembled.begin());
		std::copy(last.begin() + headerBytes, last.end(), assembled.begin() + splitAt);
		result.peakAssemblyBytes = std::max(result.peakAssemblyBytes, otherStreamsHeld + first.size() + last.size() + assembled.size());
		Discard();
		return Complete(sequence, assembled.data(), ms);
	}

private:
	bool Complete(unsigned sequence, const unsigned char *data, unsigned ms)
	{
		for (unsigned i = 0; i < stateBytes; ++i)
			if (data[i] != Value(sequence, i))
				return false;
		if (sequence > warmPublications)
			++result.completed;
		result.firstCompleted |= sequence == 1;
		if (sequence > result.latest)
		{
			result.latest = sequence;
			if (sequence > warmPublications)
				++result.applied;
			if (sequence == 2)
				result.secondAppliedAtMs = ms;
		}
		return true;
	}
	void Discard()
	{
		Bytes{}.swap(pending);
		result.remainingPartialBytes = 0;
	}
	Result &result;
	Bytes pending;
	const unsigned splitAt;
	unsigned newest = 0, began = 0, warmPublications = 0;
};
}

Result Run(Policy policy, unsigned hz, unsigned lossPercent, unsigned seed, unsigned durationMs, Drop drop, unsigned deadlineMs, unsigned warmupMs)
{
	Result result;
	if (hz != 30 && hz != 60)
		return result;
	NativeAssembly native;
	SendQueueProbe queue(hz);
	SimulatedNetwork::LinkRule path;
	bool dropArmed = drop != Drop::None;
	bool measuring = false;
	SystemAddress source, destination;
	SimulatedNetwork net(seed);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	// Negotiate and calibrate at the measured RTT so the transport starts with a representative RTT estimate.
	path.delayMs = 50;
	net.SetLinkBothWays(server, client, path);
	if (!net.Connect(client, server))
		return result;
	source = net.AddressOf(server);
	destination = net.AddressOf(client);
	const unsigned splitAt = Calibrate(net, *server, *client, native);
	if (!splitAt)
		return result;
	server->AttachPlugin(&queue);
	Receiver receiver(result, splitAt);
	unsigned warmCount = 0;
	const unsigned cutoff = warmupMs + durationMs;
	path.lossPercent = static_cast<int>(lossPercent);
	path.reorderPerThousand = drop != Drop::None ? 0 : 5;
	path.reorderDelayMs = 25;
	net.SetLinkBothWays(server, client, path);
	server->SetPerConnectionOutgoingBandwidthLimit(256 * 1024 * 8);
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *, int bytes)
	           {
		if (measuring)
			result.udpBytes += bytes;
		if (from == source && drop != Drop::None)
		{
			// The two data packets have distinct sizes. Control packets are below 350 bytes.
			const bool target = drop == Drop::FirstPart ? bytes > 1000 : bytes >= 350 && bytes <= 1000;
			path.dark = dropArmed && target;
			if (path.dark)
			{
				dropArmed = false;
				++result.forcedDrops;
			}
			net.SetLink(source, destination, path);
		} });
	std::vector<unsigned> ages;
	for (unsigned ms = 0; ms < cutoff + deadlineMs; ++ms)
	{
		if (ms == warmupMs)
		{
			if (!native.valid || (warmupMs && !result.latest))
				return result;
			warmCount = result.publications;
			queue.warmCount = warmCount;
			queue.delays.clear();
			receiver.BeginMeasurement(warmCount);
			native.peak = native.peakPartial = native.held;
			measuring = true;
		}
		if (ms < cutoff && ms == result.publications * 1000 / hz)
		{
			++result.publications;
			if (!SendSnapshot(*server, destination, policy, result.publications, splitAt))
				return result;
		}
		queue.now = ms + 1;
		net.Step(1);
		receiver.Expire(ms + 1);
		for (auto &message : net.Inbox(client))
			if (!receiver.Receive(message, ms + 1))
				return result;
		net.Inbox(client).clear();
		if (measuring && ms < cutoff && result.latest)
		{
			const unsigned publishedAt = (result.latest - 1) * 1000 / hz;
			ages.push_back(ms + 1 - publishedAt);
		}
	}
	result.p95AgeMs = Percentile(ages, 95);
	result.p95SendQueueMs = Percentile(queue.delays, 95);
	if (policy == Policy::Promoted)
	{
		result.peakAssemblyBytes = native.peak;
		result.peakPartialBytes = native.peakPartial;
		result.remainingPartialBytes = native.held;
	}
	net.SetTap({});
	result.publications -= warmCount;
	result.valid = native.valid && (drop == Drop::None || result.forcedDrops == 1);
	return result;
}

BurstResult RunBurst(Policy policy, unsigned hz, unsigned outageMs, unsigned seed, unsigned headroomPercent)
{
	constexpr unsigned streams = 4, outageStart = 6000, afterReopen = 4000, controlPeriod = 20;
	constexpr unsigned char controlId = ID_USER_PACKET_ENUM + 3;
	BurstResult result;
	if ((hz != 30 && hz != 60) || outageMs > 1000 || (headroomPercent != 20 && headroomPercent != 60))
		return result;
	const unsigned reopen = outageStart + outageMs, finish = reopen + afterReopen;
	const unsigned totalHz = streams * hz;
	// Payload offered by the snapshots and controls plus headroom for headers and recovery.
	const unsigned linkRate = (totalHz * stateBytes + 96 * 1000 / controlPeriod) * (100 + headroomPercent) / 100;
	SimulatedBottleneck fifo(linkRate / 10, {{0, linkRate}});
	NativeAssembly native;
	SimulatedNetwork net(seed);
	auto *sender = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	SimulatedNetwork::LinkRule path;
	path.delayMs = 50;
	net.SetLinkBothWays(sender, client, path);
	if (!net.Connect(client, sender))
		return result;
	const auto destination = net.AddressOf(client);
	const unsigned splitAt = Calibrate(net, *sender, *client, native);
	if (!splitAt)
		return result;
	native.peak = 0;
	path.reorderPerThousand = 5;
	path.reorderDelayMs = 25;
	net.SetLinkBothWays(sender, client, path);
	path.bottleneck = &fifo;
	net.SetLink(sender, client, path);

	std::array<Result, streams> states{};
	std::vector<Receiver> receivers;
	receivers.reserve(streams);
	for (auto &state : states)
		receivers.emplace_back(state, splitAt);
	std::array<bool, streams> fresh{};
	unsigned freshCount = 0, published = 0;
	std::vector<unsigned> latencies;
	uint64_t dropsAtStart = 0;
	for (unsigned ms = 0; ms < finish + 10000; ++ms)
	{
		if (ms == outageStart)
		{
			if (!result.reliableDelivered || std::any_of(states.begin(), states.end(), [](const Result &state)
			                                             { return !state.applied; }))
				return result;
			dropsAtStart = net.DatagramsDropped();
			path.dark = true;
			net.SetLink(sender, client, path);
		}
		if (ms == reopen)
		{
			result.outageDrops = net.DatagramsDropped() - dropsAtStart;
			path.dark = false;
			net.SetLink(sender, client, path);
		}
		if (ms < finish && ms % controlPeriod == 0)
		{
			const auto data = Encode(result.reliableSent, 0, 96 - headerBytes, controlId);
			if (!sender->Send(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), Priority::High, Reliability::ReliableOrdered, 0, destination, false))
				return result;
			++result.reliableSent;
		}
		// The sequence identifies both publication time and stream, in round-robin order.
		if (ms < finish && ms == published * 1000 / totalHz)
		{
			states[published % streams].publications = published + 1;
			++published;
			if (!SendSnapshot(*sender, destination, policy, published, splitAt))
				return result;
		}
		net.Step(1);
		const unsigned now = ms + 1;
		for (auto &receiver : receivers)
			receiver.Expire(now);
		for (auto &message : net.Inbox(client))
		{
			if (message.id < ID_USER_PACKET_ENUM)
				continue;
			if (message.id == controlId)
			{
				const unsigned expected = result.reliableDelivered;
				if (expected >= result.reliableSent || message.data != Encode(expected, 0, 96 - headerBytes, controlId))
					return result;
				++result.reliableDelivered;
				const unsigned sentAt = expected * controlPeriod;
				if (sentAt >= outageStart - 100 && sentAt < reopen + 2000)
					latencies.push_back(now - sentAt);
				continue;
			}
			if (message.data.size() < headerBytes)
				return result;
			uint32_t sequence;
			std::memcpy(&sequence, message.data.data() + 1, sizeof(sequence));
			if (!sequence || sequence > published)
				return result;
			const unsigned stream = (sequence - 1) % streams;
			auto &state = states[stream];
			size_t otherHeld = 0;
			for (unsigned i = 0; i < streams; ++i)
				if (i != stream)
					otherHeld += states[i].remainingPartialBytes;
			// Include simultaneous partials in other streams and the current receive's output copy.
			if (!receivers[stream].Receive(message, now, otherHeld))
				return result;
			result.peakAssemblyBytes = std::max(result.peakAssemblyBytes, state.peakAssemblyBytes);
			if (!fresh[stream] && state.latest && (state.latest - 1) * 1000 / totalHz >= reopen)
			{
				if (!freshCount)
					result.firstFreshMs = now - reopen;
				fresh[stream] = true;
				if (++freshCount == streams)
					result.allFreshMs = now - reopen;
			}
		}
		net.Inbox(client).clear();
	}
	unsigned completed = 0;
	for (const auto &state : states)
	{
		completed += state.completed;
		if (state.remainingPartialBytes)
			return result;
	}
	if (policy == Policy::Promoted && (completed != published || native.held != 0))
		return result;
	result.fifoDrops = fifo.drops;
	result.latencySamples = static_cast<unsigned>(latencies.size());
	result.reliableP99Ms = Percentile(latencies, 99);
	if (policy == Policy::Promoted)
		result.peakAssemblyBytes = native.peak;
	result.valid = native.valid && freshCount == streams && result.reliableDelivered == result.reliableSent &&
	               result.latencySamples > 0 && (!outageMs || result.outageDrops > 0);
	return result;
}
}
