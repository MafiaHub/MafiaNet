/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/StatisticsHistory.h"
#include "mafianet/peerinterface.h"

using namespace MafiaNet;

namespace
{
	// An idle peer: the plugin can sample it, and it has no connections to sample.
	class StatisticsHistorySampling : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			peer = RakPeerInterface::GetInstance();
			peer->AttachPlugin(&plugin);
		}
		void TearDown() override
		{
			peer->DetachPlugin(&plugin);
			RakPeerInterface::DestroyInstance(peer);
		}

		RakPeerInterface *peer = nullptr;
		StatisticsHistoryPlugin plugin;
	};
}

TEST_F(StatisticsHistorySampling, SamplesEveryUpdateByDefault)
{
	EXPECT_TRUE(plugin.UpdateAt(1000));
	EXPECT_TRUE(plugin.UpdateAt(1000));
	EXPECT_TRUE(plugin.UpdateAt(1001));
}

TEST_F(StatisticsHistorySampling, IntervalSkipsUpdatesInsideIt)
{
	plugin.SetSampleInterval(100);
	EXPECT_TRUE(plugin.UpdateAt(1000)) << "The first update always samples";
	EXPECT_FALSE(plugin.UpdateAt(1000));
	EXPECT_FALSE(plugin.UpdateAt(1099));
	EXPECT_TRUE(plugin.UpdateAt(1100));
	EXPECT_FALSE(plugin.UpdateAt(1199));
}

TEST_F(StatisticsHistorySampling, LateUpdateTakesOneSampleWithoutCatchingUp)
{
	plugin.SetSampleInterval(100);
	EXPECT_TRUE(plugin.UpdateAt(1000));
	EXPECT_TRUE(plugin.UpdateAt(5000));
	EXPECT_FALSE(plugin.UpdateAt(5001)) << "The interval restarts from the late sample";
}

TEST_F(StatisticsHistorySampling, ClockSteppingBackwardsSamplesAtOnce)
{
	plugin.SetSampleInterval(100);
	EXPECT_TRUE(plugin.UpdateAt(5000));
	EXPECT_TRUE(plugin.UpdateAt(4000));
}
