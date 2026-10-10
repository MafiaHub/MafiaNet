/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <vector>

#include "mafianet/tcp_interface.h"

#if _RAKNET_SUPPORT_TCPInterface == 1

using namespace MafiaNet;

/*
Description:
Covers TCPInterface's two pushback queues before they move off DataStructures::Queue
(std migration stage 2, #60). TCPInterface had no tests.

PushBackPacket(packet, pushAtHead) routes into one of two queues and Receive() drains headPush
before tailPush, so the pair decides the order a caller sees packets it handed back. Both the
queues and Receive() are reachable without starting the interface, so no socket is involved.

The queues are FIFO within themselves: DataStructures::Queue::Push appends at the tail and Pop
takes from the head. A migration to std::vector with push_back/pop_back would reverse each queue
while still draining head before tail, and would pass any test that only pushed one packet per
queue -- so the cases below push several.
*/

namespace
{
	class TCPInterfacePushback : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			// ReceiveInt() returns nothing until the interface is started. With
			// maxIncomingConnections of 0, Start() creates no listen socket -- it only brings up
			// the update thread -- so there is no bind, no port and no traffic. That thread feeds
			// incomingMessages; headPush and tailPush are touched only by the calling thread, so
			// the assertions below stay synchronous.
			ASSERT_TRUE(tcp.Start(0, 0));
		}
		void TearDown() override
		{
			// Drain anything a failed assertion left behind, so no packet leaks.
			Packet *p;
			while ((p = tcp.Receive()) != 0)
				tcp.DeallocatePacket(p);
			tcp.Stop();
		}

		// A packet carrying one identifying byte.
		Packet *Make(unsigned char tag)
		{
			Packet *p = tcp.AllocatePacket(1);
			p->data[0] = tag;
			return p;
		}

		std::vector<unsigned char> DrainTags(void)
		{
			std::vector<unsigned char> out;
			Packet *p;
			while ((p = tcp.Receive()) != 0)
			{
				out.push_back(p->data[0]);
				tcp.DeallocatePacket(p);
			}
			return out;
		}

		TCPInterface tcp;
	};
}

TEST_F(TCPInterfacePushback, NothingPushedReceivesNothing)
{
	EXPECT_FALSE(tcp.ReceiveHasPackets());
	EXPECT_EQ(tcp.Receive(), (Packet *) 0);
}

TEST_F(TCPInterfacePushback, APushedPacketComesBack)
{
	tcp.PushBackPacket(Make(7), false);
	EXPECT_TRUE(tcp.ReceiveHasPackets());

	std::vector<unsigned char> tags = DrainTags();
	ASSERT_EQ(tags.size(), 1u);
	EXPECT_EQ(tags[0], 7);
}

TEST_F(TCPInterfacePushback, TailPushedPacketsKeepTheirOrder)
{
	tcp.PushBackPacket(Make(1), false);
	tcp.PushBackPacket(Make(2), false);
	tcp.PushBackPacket(Make(3), false);

	std::vector<unsigned char> tags = DrainTags();
	ASSERT_EQ(tags.size(), 3u);
	EXPECT_EQ(tags[0], 1) << "the tail queue is FIFO, not a stack";
	EXPECT_EQ(tags[1], 2);
	EXPECT_EQ(tags[2], 3);
}

TEST_F(TCPInterfacePushback, HeadPushedPacketsKeepTheirOrderAmongThemselves)
{
	tcp.PushBackPacket(Make(1), true);
	tcp.PushBackPacket(Make(2), true);
	tcp.PushBackPacket(Make(3), true);

	std::vector<unsigned char> tags = DrainTags();
	ASSERT_EQ(tags.size(), 3u);
	EXPECT_EQ(tags[0], 1) << "the head queue is FIFO within itself, not a stack";
	EXPECT_EQ(tags[1], 2);
	EXPECT_EQ(tags[2], 3);
}

TEST_F(TCPInterfacePushback, HeadPushedPacketsArriveBeforeTailPushedOnes)
{
	tcp.PushBackPacket(Make(10), false); // tail
	tcp.PushBackPacket(Make(11), false); // tail
	tcp.PushBackPacket(Make(20), true);  // head
	tcp.PushBackPacket(Make(21), true);  // head

	std::vector<unsigned char> tags = DrainTags();
	ASSERT_EQ(tags.size(), 4u);
	EXPECT_EQ(tags[0], 20) << "head-pushed packets must be drained first";
	EXPECT_EQ(tags[1], 21);
	EXPECT_EQ(tags[2], 10);
	EXPECT_EQ(tags[3], 11);
}

TEST_F(TCPInterfacePushback, ReceiveHasPacketsTracksBothQueues)
{
	EXPECT_FALSE(tcp.ReceiveHasPackets());
	tcp.PushBackPacket(Make(1), true);
	EXPECT_TRUE(tcp.ReceiveHasPackets());
	Packet *p = tcp.Receive();
	ASSERT_NE(p, (Packet *) 0);
	tcp.DeallocatePacket(p);
	EXPECT_FALSE(tcp.ReceiveHasPackets());

	tcp.PushBackPacket(Make(2), false);
	EXPECT_TRUE(tcp.ReceiveHasPackets());
	p = tcp.Receive();
	ASSERT_NE(p, (Packet *) 0);
	tcp.DeallocatePacket(p);
	EXPECT_FALSE(tcp.ReceiveHasPackets());
}

TEST_F(TCPInterfacePushback, ManyPacketsSurviveInOrder)
{
	const int count = 50;
	for (int i = 0; i < count; ++i)
		tcp.PushBackPacket(Make((unsigned char) i), false);

	std::vector<unsigned char> tags = DrainTags();
	ASSERT_EQ(tags.size(), (size_t) count);
	for (int i = 0; i < count; ++i)
		EXPECT_EQ(tags[i], (unsigned char) i) << "at position " << i;
}

#endif // _RAKNET_SUPPORT_TCPInterface
