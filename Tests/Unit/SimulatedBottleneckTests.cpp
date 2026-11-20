// Copyright (c) 2026, MafiaHub. Licensed under the MIT-style license in license.txt.
#include "SimulatedBottleneck.h"
#include <gtest/gtest.h>

using namespace MafiaNet;

TEST(SimulatedBottleneck, SharedFifoSerializesPacketsAndDropsAtTheByteLimit)
{
	SimulatedBottleneck queue(1500, {{0, 1000000}});
	TimeUS departure = 0;
	ASSERT_TRUE(queue.Admit(100, 1000, departure));
	EXPECT_EQ(departure, 1100u);
	ASSERT_TRUE(queue.Admit(200, 500, departure));
	EXPECT_EQ(departure, 1600u);
	EXPECT_FALSE(queue.Admit(1000, 1, departure));
	EXPECT_EQ(queue.QueuedBytes(1100), 500u);
	EXPECT_TRUE(queue.Admit(1100, 1000, departure));
	EXPECT_EQ(departure, 2600u);
	EXPECT_EQ(queue.QueuedBytes(2600), 0u);
	EXPECT_EQ(queue.peakBytes, 1500u);
	EXPECT_EQ(queue.drops, 1u);
	EXPECT_EQ(queue.droppedBytes, 1u);
	EXPECT_EQ(queue.admittedBytes, 2500u);
	EXPECT_EQ(queue.waitUs, (std::vector<TimeUS>{0, 900, 500}));
}

TEST(SimulatedBottleneck, PacketSpanningCapacityChangesUsesEachRate)
{
	SimulatedBottleneck queue(4096, {{0, 1000000}, {1000, 250000}, {3000, 1000000}});
	TimeUS departure = 0;
	ASSERT_TRUE(queue.Admit(0, 2000, departure));
	EXPECT_EQ(departure, 3500u);
	ASSERT_TRUE(queue.Admit(2000, 500, departure));
	EXPECT_EQ(departure, 4000u);
	EXPECT_EQ(queue.QueuedBytes(3499), 2500u);
	EXPECT_EQ(queue.QueuedBytes(3500), 500u);
}
