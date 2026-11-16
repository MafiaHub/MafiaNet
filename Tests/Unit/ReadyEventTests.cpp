/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// ReadyEvent keeps its events sorted by id and each event's wait list sorted by guid; the index
// accessors expose that order. A started peer with no connections: nothing is sent anywhere.

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/ready_event.h"

using namespace MafiaNet;

namespace
{
class ReadyEventLists : public ::testing::Test
{
protected:
	void SetUp() override
	{
		peer = net.AddPeer(4, 4);
		ASSERT_NE(peer, nullptr);
		peer->AttachPlugin(&readyEvent);
	}

	void TearDown() override
	{
		if (peer != nullptr)
			peer->DetachPlugin(&readyEvent);
	}

	ReadyEvent readyEvent;
	SimulatedNetwork net;
	RakPeer *peer = nullptr;
};
} // namespace

TEST_F(ReadyEventLists, EventsAreIndexedInIdOrder)
{
	ASSERT_TRUE(readyEvent.SetEvent(50, true));
	ASSERT_TRUE(readyEvent.SetEvent(-3, false));
	ASSERT_TRUE(readyEvent.SetEvent(7, true));

	ASSERT_EQ(readyEvent.GetEventListSize(), 3u);
	EXPECT_EQ(readyEvent.GetEventAtIndex(0), -3);
	EXPECT_EQ(readyEvent.GetEventAtIndex(1), 7);
	EXPECT_EQ(readyEvent.GetEventAtIndex(2), 50);
	EXPECT_TRUE(readyEvent.HasEvent(7));
	EXPECT_FALSE(readyEvent.HasEvent(8));
	EXPECT_TRUE(readyEvent.IsEventSet(50));
	EXPECT_FALSE(readyEvent.IsEventSet(-3));

	EXPECT_TRUE(readyEvent.DeleteEvent(7));
	EXPECT_FALSE(readyEvent.DeleteEvent(7));
	ASSERT_EQ(readyEvent.GetEventListSize(), 2u);
	EXPECT_EQ(readyEvent.GetEventAtIndex(0), -3);
	EXPECT_EQ(readyEvent.GetEventAtIndex(1), 50);
}

TEST_F(ReadyEventLists, WaitListIsIndexedInGuidOrder)
{
	ASSERT_TRUE(readyEvent.AddToWaitList(1, RakNetGUID(300)));
	ASSERT_TRUE(readyEvent.AddToWaitList(1, RakNetGUID(100)));
	ASSERT_TRUE(readyEvent.AddToWaitList(1, RakNetGUID(200)));
	EXPECT_FALSE(readyEvent.AddToWaitList(1, RakNetGUID(200))) << "a waiter is listed once";

	ASSERT_EQ(readyEvent.GetRemoteWaitListSize(1), 3u);
	EXPECT_EQ(readyEvent.GetFromWaitListAtIndex(1, 0), RakNetGUID(100));
	EXPECT_EQ(readyEvent.GetFromWaitListAtIndex(1, 1), RakNetGUID(200));
	EXPECT_EQ(readyEvent.GetFromWaitListAtIndex(1, 2), RakNetGUID(300));
	EXPECT_TRUE(readyEvent.IsInWaitList(1, RakNetGUID(200)));
	EXPECT_FALSE(readyEvent.IsInWaitList(1, RakNetGUID(250)));

	EXPECT_TRUE(readyEvent.RemoveFromWaitList(1, RakNetGUID(200)));
	EXPECT_FALSE(readyEvent.IsInWaitList(1, RakNetGUID(200)));
	ASSERT_EQ(readyEvent.GetRemoteWaitListSize(1), 2u);
	EXPECT_EQ(readyEvent.GetFromWaitListAtIndex(1, 0), RakNetGUID(100));
	EXPECT_EQ(readyEvent.GetFromWaitListAtIndex(1, 1), RakNetGUID(300));
	EXPECT_EQ(readyEvent.GetRemoteWaitListSize(2), 0u) << "no such event";
}
