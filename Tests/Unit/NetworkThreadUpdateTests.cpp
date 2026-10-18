/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  PluginInterface2::UpdateNetworkThread: every attached plugin is ticked once
 *  per RakPeer update cycle, whether or not the
 *  application ever calls Receive(). Run on the SimulatedNetwork with Receive()
 *  switched off, which is what a stalled application main thread looks like.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;

namespace
{
class UpdateProbe : public PluginInterface2
{
public:
	void UpdateNetworkThread(void) override { ++networkThreadUpdates; }
	void Update(void) override { ++userThreadUpdates; }

	int networkThreadUpdates = 0;
	int userThreadUpdates = 0;
};
} // namespace

TEST(NetworkThreadUpdate, TicksEveryCycleWhileReceiveIsStalled)
{
	SimulatedNetwork net;
	RakPeer *peer = net.AddPeer(1);
	UpdateProbe probe;
	peer->AttachPlugin(&probe);
	net.SetReceiveEnabled(peer, false);

	net.Step(50); // 50 one-millisecond update cycles
	EXPECT_EQ(50, probe.networkThreadUpdates);
	EXPECT_EQ(0, probe.userThreadUpdates);
	peer->DetachPlugin(&probe);
}

TEST(NetworkThreadUpdate, DetachedPluginIsNoLongerTicked)
{
	SimulatedNetwork net;
	RakPeer *peer = net.AddPeer(1);
	UpdateProbe probe;
	peer->AttachPlugin(&probe);
	net.Step(5);
	peer->DetachPlugin(&probe);
	const int before = probe.networkThreadUpdates;

	net.Step(20);
	EXPECT_EQ(before, probe.networkThreadUpdates);
}
