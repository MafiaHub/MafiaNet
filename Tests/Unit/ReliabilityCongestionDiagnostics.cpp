/*
 *  Diagnostic harness for the fragmentation cliff: traces the congestion
 *  window and the sender queue of the fragmentation workload, split arm
 *  against unsplit arm. Prints a trajectory and asserts nothing, so it only
 *  runs when MAFIANET_FRAGMENTATION_DIAGNOSTICS is set and the default unit
 *  suite never pays for it. Temporary: removed once the cliff is understood.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"
#include "mafianet/peer.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_DIAG = ID_USER_PACKET_ENUM + 90;

struct Arm
{
	const char *name;
	int loss;
	bool split;
	bool reliable;
};

void RunArm(const Arm &arm, int durationMs)
{
	SimulatedNetwork net(1);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	if (!net.Connect(client, server))
	{
		printf("%s: connect failed\n", arm.name);
		return;
	}
	SimulatedNetwork::LinkRule link;
	link.delayMs = 50;
	link.lossPercent = arm.loss;
	link.reorderPerThousand = 5;
	link.reorderDelayMs = 25;
	net.SetLinkBothWays(server, client, link);
	server->SetPerConnectionOutgoingBandwidthLimit(256 * 1024 * 8);

	const auto address = net.AddressOf(client);
	const int limit = net.MaxUnsplitPayload(server, address);
	const int bytes = limit + (arm.split ? 1 : 0);
	const int messages = 3 * (arm.split ? limit : limit + 1);

	std::vector<unsigned char> payload(bytes, 0x5A);
	payload[0] = ID_DIAG;

	printf("%s: limit=%d bytes=%d messages=%d over %d ms\n", arm.name, limit, bytes, messages, durationMs);

	int delivered = 0;
	int sent = 0;

	// Event accounting for the congestion window trajectory.
	uint32_t prevCwnd = 0;
	bool havePrev = false;
	int dropsToMtu = 0; // cwnd collapsed to exactly one MTU: an OnResend backoff
	int otherDrops = 0; // cwnd fell some other way
	int rises = 0;
	uint32_t minCwnd = 0xFFFFFFFF;
	uint32_t maxCwnd = 0;
	long long cwndSum = 0;
	long long cwndSamples = 0;
	int slowStartSamples = 0;
	int capBlockedSamples = 0;
	int deliveredAtHalf = 0;
	uint32_t queueAtEnd = 0;

	for (int ms = 0; ms < durationMs; ++ms)
	{
		const auto due = static_cast<size_t>(static_cast<int64_t>(ms + 1) * messages / durationMs);
		while (sent < static_cast<int>(due))
		{
			server->Send(reinterpret_cast<const char *>(payload.data()), bytes, Priority::High,
			             arm.reliable ? Reliability::Reliable : Reliability::Unreliable, 0, address, false);
			++sent;
		}
		net.Step(1);
		for (const auto &m : net.Inbox(client))
			if (m.id == ID_DIAG)
				++delivered;
		net.Inbox(client).clear();

		SimulatedNetwork::SendBudget budget;
		if (!net.ReadSendBudget(server, address, budget))
			break;

		if (havePrev)
		{
			if (budget.windowBytes < prevCwnd)
			{
				// One MTU is the congestion controller's floor; a drop onto it is a
				// timeout backoff, anything else is a threshold reset.
				if (budget.windowBytes <= 1347)
					++dropsToMtu;
				else
					++otherDrops;
			}
			else if (budget.windowBytes > prevCwnd)
				++rises;
		}
		havePrev = true;
		prevCwnd = budget.windowBytes;
		minCwnd = std::min(minCwnd, budget.windowBytes);
		maxCwnd = std::max(maxCwnd, budget.windowBytes);
		cwndSum += budget.windowBytes;
		++cwndSamples;
		if (budget.slowStart)
			++slowStartSamples;
		if (budget.capBlocked)
			++capBlockedSamples;

		if (ms == durationMs - 20001)
			deliveredAtHalf = delivered;
		if (ms == durationMs - 1)
			queueAtEnd = budget.queuedBytes;

		if (ms % 2000 == 0)
		{
			printf("%s t=%6d cwnd=%7u inflight=%7u queued=%9u slow=%d rtt=%6.1f delivered=%6d offered=%6d\n",
			       arm.name, ms, budget.windowBytes, budget.inFlightBytes, budget.queuedBytes, (int) budget.slowStart,
			       budget.rttMillis, delivered, sent);
		}
	}

	printf("%s: delivered=%d of %d offered; cwnd min=%u max=%u avg=%lld dropsToMtu=%d otherDrops=%d rises=%d slowSamples=%d/%lld capBlocked=%d\n",
	       arm.name, delivered, messages, minCwnd, maxCwnd, cwndSamples ? cwndSum / cwndSamples : 0, dropsToMtu, otherDrops, rises,
	       slowStartSamples, cwndSamples, capBlockedSamples);
	// Steady state over the last 20 s: delivered rate and the queue it leaves behind.
	printf("%s: steady delivered=%d in 20 s (%.1f/s), queue at end=%u bytes\n", arm.name, delivered - deliveredAtHalf,
	       (delivered - deliveredAtHalf) / 20.0, queueAtEnd);
}
} // namespace

TEST(Diagnostics, FragmentationWindowTrajectory)
{
	if (std::getenv("MAFIANET_FRAGMENTATION_DIAGNOSTICS") == nullptr)
		GTEST_SKIP() << "fragmentation-cliff trace: set MAFIANET_FRAGMENTATION_DIAGNOSTICS=1 to run the four 60-second arms";
	RunArm({"unsplit unrel", 2, false, false}, 60000);
	RunArm({"unsplit rel  ", 2, false, true}, 60000);
	RunArm({"split        ", 2, true, false}, 60000);
	RunArm({"split 0loss  ", 0, true, false}, 60000);
}
