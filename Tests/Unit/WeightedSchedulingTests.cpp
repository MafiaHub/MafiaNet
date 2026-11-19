/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <array>
#include "SimulatedNetwork.h"
#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
struct Fragment
{
	Priority priority;
	unsigned bytes;
};

class SchedulerTrace : public PluginInterface2
{
public:
	bool UsesReliabilityLayer() const override { return true; }
	void OnInternalPacket(InternalPacket *packet, unsigned, SystemAddress, TimeMS, int isSend) override
	{
		if (!isSend)
			return;
		const auto *message = packet->splitPacketCount ? packet->refCountedData->sharedDataBlock : packet->data;
		if (message[0] != ID_USER_PACKET_ENUM)
			return;
		if (packet->timesSent == 1)
			firstSends.push_back({packet->priority, BITS_TO_BYTES(packet->dataBitLength)});
		else
			++retries;
	}
	std::vector<Fragment> firstSends;
	unsigned retries = 0;
};

class WeightedScheduling : public testing::Test
{
protected:
	void Connect(uint8_t highWeight = 3)
	{
		from = net.AddPeer(1);
		to = net.AddPeer(1, 1);
		ASSERT_TRUE(from->SetOutgoingSchedulingPolicy({OutgoingSchedulingMode::WeightedBytes, highWeight, 2, 1}));
		ASSERT_TRUE(net.Connect(from, to));
		from->SetPerConnectionOutgoingBandwidthLimit(1024 * 1024);
		from->AttachPlugin(&trace);
		net.Inbox(to).clear();
	}

	void Send(Priority priority, int bytes, int channel, unsigned char tag)
	{
		std::vector<char> data(bytes, static_cast<char>(tag));
		data.front() = static_cast<char>(ID_USER_PACKET_ENUM);
		ASSERT_NE(from->Send(data.data(), bytes, priority, Reliability::ReliableOrdered, static_cast<char>(channel), net.AddressOf(to), false), 0u);
	}

	SchedulerTrace trace;
	SimulatedNetwork net;
	RakPeer *from = nullptr, *to = nullptr;
};
}

TEST_F(WeightedScheduling, PayloadSharesBoundedAtEveryBackloggedFragment)
{
	ASSERT_NO_FATAL_FAILURE(Connect());
	// All priorities have enough bytes to remain backlogged for the interval.
	// Mix split and unsplit traffic and use two streams at the same priority.
	Send(Priority::Low, 256 * 1024, 3, 40);
	for (int i = 0; i < 260; ++i)
	{
		Send(Priority::Medium, 750, 2, 30);
		Send(Priority::High, 180, 0, 10);
		Send(Priority::High, 1900, 1, 20);
	}
	Send(Priority::Immediate, 60, 4, 50);
	net.Step(1000);
	ASSERT_FALSE(trace.firstSends.empty());
	EXPECT_EQ(trace.firstSends.front().priority, Priority::Immediate);
	std::array<int, NUMBER_OF_PRIORITIES> bytes{};
	for (const auto &fragment : trace.firstSends)
	{
		bytes[static_cast<int>(fragment.priority)] += static_cast<int>(fragment.bytes);
		// Normalized service agrees to within a fragment on each side, even
		// before the large low-priority message has finished reassembly.
		ASSERT_NEAR(bytes[1] / 3.0, bytes[3], 2 * MAXIMUM_MTU_SIZE);
		ASSERT_NEAR(bytes[2] / 2.0, bytes[3], 2 * MAXIMUM_MTU_SIZE);
	}
	EXPECT_GT(bytes[3], 12000);
}

TEST_F(WeightedScheduling, IdleClassCannotBankCreditAndSoleSenderBorrowsCapacity)
{
	ASSERT_NO_FATAL_FAILURE(Connect(7));
	Send(Priority::Low, 512 * 1024, 1, 40);
	net.Step(700);
	size_t start = trace.firstSends.size();
	unsigned soloBytes = 0;
	for (const auto &fragment : trace.firstSends)
		soloBytes += fragment.bytes;
	EXPECT_GT(soloBytes, 65000u); // a quarter-share reservation would send < 24 KiB

	Send(Priority::High, 512 * 1024, 0, 10);
	net.Step(700);
	unsigned high = 0, low = 0;
	for (size_t i = start; i < trace.firstSends.size(); ++i)
	{
		const auto &fragment = trace.firstSends[i];
		if (fragment.priority == Priority::High)
			high += fragment.bytes;
		else if (fragment.priority == Priority::Low)
			low += fragment.bytes;
		EXPECT_NEAR(high / 7.0, low, 2 * MAXIMUM_MTU_SIZE);
	}
	EXPECT_GT(high, 50000u);
	EXPECT_GT(low, 8000u);
}

TEST_F(WeightedScheduling, FragmentRetriesCompleteEachOrderedStreamUnderLoss)
{
	ASSERT_NO_FATAL_FAILURE(Connect());
	SimulatedNetwork::LinkRule link;
	link.lossPercent = 12;
	link.jitterMs = 7;
	net.SetLinkBothWays(from, to, link);
	for (unsigned char tag = 1; tag <= 6; ++tag)
	{
		Send(Priority::Low, 18000, 1, tag);
		Send(Priority::High, 2200, 0, tag);
	}
	auto &inbox = net.Inbox(to);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return std::count_if(inbox.begin(), inbox.end(), [](const auto &message)
		                                            { return message.id == ID_USER_PACKET_ENUM; }) == 12; }, 60000));
	unsigned low = 0, high = 0;
	for (const auto &message : inbox)
	{
		if (message.id != ID_USER_PACKET_ENUM)
			continue;
		ASSERT_TRUE(message.data.size() == 18000 || message.data.size() == 2200);
		const auto next = message.data.size() == 18000 ? ++low : ++high;
		EXPECT_TRUE(std::all_of(message.data.begin() + 1, message.data.end(), [next](unsigned char byte)
		                        { return byte == next; }));
	}
	EXPECT_EQ(low, 6u);
	EXPECT_EQ(high, 6u);
	EXPECT_GT(trace.retries, 0u);
}


TEST_F(WeightedScheduling, ZeroWeightsAreRejected)
{
	auto *peer = net.AddPeer(1);
	EXPECT_FALSE(peer->SetOutgoingSchedulingPolicy({OutgoingSchedulingMode::WeightedBytes, 0, 2, 1}));
	EXPECT_FALSE(peer->SetOutgoingSchedulingPolicy({OutgoingSchedulingMode::WeightedBytes, 3, 0, 1}));
	EXPECT_FALSE(peer->SetOutgoingSchedulingPolicy({OutgoingSchedulingMode::WeightedBytes, 3, 2, 0}));
}
