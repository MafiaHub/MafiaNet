// Copyright (c) 2026, MafiaHub. Licensed under the MIT license.
#include <gtest/gtest.h>
#include "SimulatedNetwork.h"
#include "mafianet/latest_state_queue.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

TEST(LatestStateQueue, CoalescesByObjectPreservesOrderAndRetriesPendingOnFailure)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto target = net.AddressOf(client);
	const auto budget = LatestStateQueue::PayloadBudget(server->GetMTUSize(target));
	LatestStateQueue queue;
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
	EXPECT_EQ(queue.SendOne(*server, UNASSIGNED_SYSTEM_ADDRESS, ID_USER_PACKET_ENUM, key), LatestStateQueue::Result::Disconnected);
	ASSERT_EQ(queue.Size(), 2u);
	ASSERT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), LatestStateQueue::Result::Queued);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_USER_PACKET_ENUM, 1000));
	const auto &bytes = net.Inbox(client).back().data;
	const std::vector<unsigned char> expected{ID_USER_PACKET_ENUM, 0, 1, 3, 0, 1, 2};
	EXPECT_EQ(bytes, expected);
	EXPECT_EQ(queue.Size(), 0u);
}

TEST(LatestStateQueue, MaximumRecordFitsAndAnOversizedFrontCanBeReplaced)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const auto target = net.AddressOf(client);
	const auto budget = LatestStateQueue::PayloadBudget(server->GetMTUSize(target));
	ASSERT_GT(budget, 3u);
	std::vector<unsigned char> record(budget - 2, 42);
	LatestStateQueue queue;
	ASSERT_TRUE(queue.Put(17, record.data(), record.size(), budget + 1));
	uint64_t key = 0;
	EXPECT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), LatestStateQueue::Result::RecordTooLarge);
	EXPECT_EQ(key, 17u);
	EXPECT_FALSE(queue.Put(key, record.data(), record.size(), budget));
	record.pop_back();
	ASSERT_TRUE(queue.Put(key, record.data(), record.size(), budget));
	ASSERT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), LatestStateQueue::Result::Queued);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_USER_PACKET_ENUM, 1000));
	const auto &bytes = net.Inbox(client).back().data;
	ASSERT_EQ(bytes.size(), budget);
	const unsigned char *cursor = bytes.data() + 1;
	size_t remaining = bytes.size() - 1;
	const unsigned char *data;
	size_t length;
	ASSERT_TRUE(LatestStateQueue::ReadRecord(cursor, remaining, data, length));
	EXPECT_TRUE(std::equal(record.begin(), record.end(), data, data + length));
	EXPECT_EQ(remaining, 0u);
	EXPECT_FALSE(LatestStateQueue::ReadRecord(cursor, remaining, data, length));
}

TEST(LatestStateQueue, IncompleteRecordPreservesCursor)
{
	const unsigned char bytes[]{0, 4, 1, 2};
	const unsigned char *cursor = bytes;
	size_t remaining = sizeof(bytes);
	const unsigned char *record;
	size_t size;
	EXPECT_FALSE(LatestStateQueue::ReadRecord(cursor, remaining, record, size));
	EXPECT_EQ(cursor, bytes);
	EXPECT_EQ(remaining, sizeof(bytes));
}

TEST(LatestStateQueue, BusySenderRetainsOnlyNewestSnapshotUntilReliableBacklogClears)
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
	LatestStateQueue queue;
	const unsigned char first = 1, second = 2;
	const auto budget = LatestStateQueue::PayloadBudget(server->GetMTUSize(target));
	ASSERT_TRUE(queue.Put(42, &first, 1, budget));
	uint64_t key = 0;
	ASSERT_EQ(queue.SendOne(*server, target, ID_USER_PACKET_ENUM, key), LatestStateQueue::Result::Busy);
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
			EXPECT_EQ(message.data[1], eventsReceived++);
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
