// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#include <gtest/gtest.h>
#include "SimulatedNetwork.h"
#include "mafianet/state_update_queue.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

TEST(StateUpdateQueue, CoalescesByObjectPreservesOrderAndRetriesPendingOnFailure)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto target = net.AddressOf(client);
	const auto budget = StateUpdateQueue::PayloadBudget(server->GetMTUSize(target));
	StateUpdateQueue queue(16, StateUpdateQueue::Delivery::LatestOnly);
	unsigned char value = 1;
	ASSERT_TRUE(queue.Put(123, &value, 1, budget));
	value = 2;
	ASSERT_TRUE(queue.Put(456, &value, 1, budget));
	value = 3;
	ASSERT_TRUE(queue.Put(123, &value, 1, budget));
	ASSERT_TRUE(queue.Put(789, &value, 1, budget));
	queue.Erase(789);
	ASSERT_FALSE(queue.Put(456, &value, 1, 3));
	uint64_t key = 0;
	EXPECT_EQ(queue.SendOne(*server, UNASSIGNED_SYSTEM_ADDRESS, ID_USER_PACKET_ENUM, key), StateUpdateQueue::Result::Disconnected);
	ASSERT_EQ(queue.Size(), 2u);
	ASSERT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), StateUpdateQueue::Result::Queued);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_USER_PACKET_ENUM, 1000));
	const auto &bytes = net.Inbox(client).back().data;
	const std::vector<unsigned char> expected{ID_USER_PACKET_ENUM, 0, 1, 3, 0, 1, 2};
	EXPECT_EQ(bytes, expected);
	EXPECT_EQ(queue.Size(), 0u);
}

TEST(StateUpdateQueue, MaximumRecordFitsAndAnOversizedFrontCanBeReplaced)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto target = net.AddressOf(client);
	const auto budget = StateUpdateQueue::PayloadBudget(server->GetMTUSize(target));
	ASSERT_GT(budget, 3u);
	std::vector<unsigned char> record(budget - 2, 42);
	StateUpdateQueue queue(16, StateUpdateQueue::Delivery::LatestOnly);
	ASSERT_TRUE(queue.Put(17, record.data(), record.size(), budget + 1));
	uint64_t key = 0;
	EXPECT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), StateUpdateQueue::Result::RecordTooLarge);
	EXPECT_EQ(key, 17u);
	EXPECT_FALSE(queue.Put(key, record.data(), record.size(), budget));
	record.pop_back();
	ASSERT_TRUE(queue.Put(key, record.data(), record.size(), budget));
	ASSERT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), StateUpdateQueue::Result::Queued);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_USER_PACKET_ENUM, 1000));
	const auto &bytes = net.Inbox(client).back().data;
	ASSERT_EQ(bytes.size(), budget);
	const unsigned char *cursor = bytes.data() + 1;
	size_t remaining = bytes.size() - 1;
	const unsigned char *data;
	size_t length;
	ASSERT_TRUE(StateUpdateQueue::ReadRecord(cursor, remaining, data, length));
	EXPECT_TRUE(std::equal(record.begin(), record.end(), data, data + length));
	EXPECT_EQ(remaining, 0u);
	EXPECT_FALSE(StateUpdateQueue::ReadRecord(cursor, remaining, data, length));
}

TEST(StateUpdateQueue, IncompleteRecordPreservesCursor)
{
	const unsigned char bytes[]{0, 4, 1, 2};
	const unsigned char *cursor = bytes;
	size_t remaining = sizeof(bytes);
	const unsigned char *record;
	size_t size;
	EXPECT_FALSE(StateUpdateQueue::ReadRecord(cursor, remaining, record, size));
	EXPECT_EQ(cursor, bytes);
	EXPECT_EQ(remaining, sizeof(bytes));
}

TEST(StateUpdateQueue, BusySenderRetainsOnlyNewestSnapshotUntilReliableBacklogClears)
{
	SimulatedNetwork net(12);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto target = net.AddressOf(client);
	SimulatedNetwork::LinkRule rule;
	rule.delayMs = 15;
	rule.lossPercent = 2;
	net.SetLinkBothWays(server, client, rule);
	server->SetPerConnectionOutgoingBandwidthLimit(32768 * 8);
	constexpr MessageID eventId = ID_USER_PACKET_ENUM + 1;
	std::vector<char> event(512, 0);
	event[0] = static_cast<char>(eventId);
	for (int i = 0; i < 40; ++i)
	{
		event[1] = static_cast<char>(i);
		ASSERT_NE(server->Send(event.data(), static_cast<int>(event.size()), Priority::High, Reliability::ReliableOrdered, 1, target, false), 0u);
	}
	net.Step(1);
	StateUpdateQueue queue(16, StateUpdateQueue::Delivery::LatestOnly);
	const unsigned char first = 1, second = 2;
	const auto budget = StateUpdateQueue::PayloadBudget(server->GetMTUSize(target));
	ASSERT_TRUE(queue.Put(42, &first, 1, budget));
	uint64_t key = 0;
	ASSERT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), StateUpdateQueue::Result::Busy);
	ASSERT_TRUE(queue.Put(42, &second, 1, budget));
	for (int ms = 0; ms < 5000 && queue.Size(); ++ms)
	{
		net.Step(1);
		queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key);
	}
	ASSERT_EQ(queue.Size(), 0u);
	net.Step(2000);
	unsigned eventsReceived = 0, snapshotsReceived = 0;
	for (const auto &message : net.Inbox(client))
	{
		if (message.id == eventId)
		{
			EXPECT_EQ(message.data[1], eventsReceived++);
		}
		if (message.id == ID_USER_PACKET_ENUM)
		{
			ASSERT_EQ(message.data.size(), 4u);
			EXPECT_EQ(message.data.back(), second);
			++snapshotsReceived;
		}
	}
	EXPECT_EQ(eventsReceived, 40u);
	EXPECT_EQ(snapshotsReceived, 1u);
}

TEST(StateUpdateQueue, DefaultDeliveryPreservesEveryGenerationUnderLossReorderAndBackpressure)
{
	SimulatedNetwork net(45);
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto target = net.AddressOf(client);
	SimulatedNetwork::LinkRule rule;
	rule.delayMs = 20;
	rule.lossPercent = 10;
	rule.reorderPerThousand = 100;
	rule.reorderDelayMs = 30;
	net.SetLinkBothWays(server, client, rule);
	const auto budget = StateUpdateQueue::PayloadBudget(server->GetMTUSize(target));
	StateUpdateQueue queue(8);
	std::vector<unsigned char> value(budget - 2, 0); // Simulate a smaller MTU after admission.
	for (unsigned i = 0; i < 8; ++i)
	{
		value[0] = static_cast<unsigned char>(i);
		ASSERT_TRUE(queue.Put(0, value.data(), value.size(), budget + 1));
	}
	value[0] = 8;
	ASSERT_FALSE(queue.Put(0, value.data(), value.size(), budget + 1));
	EXPECT_FALSE(queue.Erase(0));
	unsigned admitted = 8;
	uint64_t key = 0;
	for (int ms = 0; ms < 20000 && (admitted < 80 || queue.Size()); ++ms)
	{
		queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key);
		if (admitted < 80)
		{
			value[0] = static_cast<unsigned char>(admitted);
			if (queue.Put(0, value.data(), value.size(), budget + 1))
				++admitted;
		}
		net.Step(1);
	}
	net.Step(10000);
	ASSERT_EQ(admitted, 80u);
	ASSERT_EQ(queue.Size(), 0u);
	unsigned received = 0;
	for (const auto &m : net.Inbox(client))
	{
		if (m.id != ID_USER_PACKET_ENUM)
			continue;
		ASSERT_EQ(m.data.size(), value.size() + 3);
		EXPECT_EQ(m.data[3], received++);
	}
	EXPECT_EQ(received, admitted);
}
