// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "../../Source/src/replication_clock.h"
#include "../../Source/src/steady_time.h"
#include "../../Source/src/replication_wire.h"
#include "SimulatedNetwork.h"
#include "mafianet/snapshot_replica.h"

using namespace MafiaNet;
namespace
{
TEST(ReplicationClock, SteadyTimeUsesTheSimulatedClockOverride)
{
	SimulatedNetwork net;
	const auto start = GetSteadyTimeUS();
	ASSERT_EQ(start, net.Now());
	net.Step(17);
	EXPECT_EQ(GetSteadyTimeUS(), start + 17000);
}
TEST(ReplicationClock, InterpolatesFractionalTicksAcrossUnrelatedClockOrigins)
{
	ReplicationClock clock;
	EXPECT_FALSE(clock.RenderTick(0, 2));
	clock.Observe(100, 5000000, 0);
	EXPECT_FALSE(clock.RenderTick(100000, 2));
	clock.Observe(105, 5100000, 100000);
	ASSERT_EQ(clock.RenderTick(100000, 2), 103.0);
	EXPECT_EQ(clock.RenderTick(110000, 2), 103.5);
	EXPECT_EQ(clock.RenderTick(110000, 200), 0.0);
	clock.Observe(104, 5200000, 110000);
	clock.Observe(106, 5100000, 110000);
	clock.Observe(106, 5200000, 99999);
	EXPECT_EQ(clock.RenderTick(110000, 2), 103.5);
}

TEST(ReplicationClock, TracksDriftAndJitterWithoutReversingOnPacketArrival)
{
	ReplicationClock clock;
	double last = 0;
	for (unsigned i = 0; i < 600; ++i)
	{
		const TimeUS sent = 100000ull * i;
		// Receiver runs 0.3% faster, with alternating path delay and packet loss.
		const TimeUS arrived = 9000000 + 100300ull * i + (i % 3 == 0 ? 25000 : 1000);
		if (i % 7)
		{
			const auto before = clock.RenderTick(arrived, 3);
			clock.Observe(1000 + 6 * i, sent, arrived);
			if (before)
				EXPECT_GE(*clock.RenderTick(arrived, 3), *before);
		}
		const auto rendered = clock.RenderTick(arrived + 8000, 3);
		if (!rendered)
			continue;
		EXPECT_GE(*rendered, last);
		EXPECT_NEAR(*rendered, 1000 + 6 * i - 3.0, 5.0);
		last = *rendered;
	}
}

TEST(ReplicationClock, FreezesDuringSilenceAndRecoversFromPausesAndForwardJumps)
{
	ReplicationClock clock;
	clock.Observe(1, 0, 3000);
	clock.Observe(7, 100000, 103000);
	const auto held = clock.RenderTick(2000000, 2);
	ASSERT_TRUE(held);
	EXPECT_EQ(clock.RenderTick(9000000, 2), held);
	clock.Observe(8, 9000000, 9003000); // Simulation paused while local time advanced.
	EXPECT_EQ(clock.RenderTick(9003000, 2), held);
	for (unsigned i = 1; i <= 30; ++i)
		clock.Observe(8 + 6 * i, 9000000 + 100000 * i, 9003000 + 100000 * i);
	EXPECT_NEAR(*clock.RenderTick(12003000, 2), 186, 1.0);
	clock.Observe(2000, 12100000, 12103000);
	EXPECT_NEAR(*clock.RenderTick(12103000, 2), 1998, 1.0);
}

TEST(ReplicationClock, RejectsIncompleteZeroAndExtendedWireSamples)
{
	for (unsigned mode = 0; mode < 4; ++mode)
	{
		BitStream in;
		in.Write(static_cast<Time>(mode == 1 ? 0 : 123));
		if (mode != 2)
			in.Write(static_cast<TimeUS>(456));
		if (mode == 3)
			in.Write(true);
		ReplicationWire::ClockSample sample{};
		EXPECT_EQ(sample.Read(in), mode == 0);
	}
}

struct ClockState
{
	void Write(BitStream &) const {}
	bool Read(BitStream &) { return true; }
};
struct Timed : SnapshotReplica<ClockState, 4>
{
};
struct Plain : SnapshotReplica<ClockState>
{
};

TEST(ReplicationClock, SyncsWithIdleWorldAndStartsFreshAfterReconnect)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	const auto serverId = ToPeerGuid(server->GetMyGUID());
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	ASSERT_TRUE(host.RegisterReplica<Timed>(ReplicaTypeId{1}, 1));
	ASSERT_TRUE(view.RegisterReplica<Timed>(ReplicaTypeId{1}, 1));
	host.SetTickMode(ReplicationTickMode::Manual);
	view.SetTickMode(ReplicationTickMode::Manual);
	SimulatedNetwork::LinkRule link;
	link.delayMs = 30;
	link.jitterMs = 12;
	link.lossPercent = 4;
	net.SetDefaultLink(link);
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetPeerStatus(serverId) == ReplicationPeerStatus::Active; }, 5000));
	EXPECT_FALSE(view.GetRenderTick());
	for (Time tick = 1; tick <= 250; ++tick)
	{
		ASSERT_TRUE(host.Tick(tick));
		net.Step(20);
	}
	const auto rendered = view.GetRenderTick();
	ASSERT_TRUE(rendered);
	EXPECT_EQ(rendered, view.GetRenderTick(serverId));
	EXPECT_NEAR(*rendered, 247, 5);
	EXPECT_FALSE(view.GetRenderTick(ToPeerGuid(client->GetMyGUID())));
	EXPECT_FALSE(host.GetRenderTick(ToPeerGuid(client->GetMyGUID())));
	EXPECT_GT(host.GetStatistics().clockBytes, 0u);
	EXPECT_EQ(view.GetStatistics().clockBytes, 0u);
	client->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !view.GetConnectionCount() && !host.GetConnectionCount(); }, 5000));
	EXPECT_FALSE(view.GetRenderTick());
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetPeerStatus(serverId) == ReplicationPeerStatus::Active; }, 5000));
	EXPECT_FALSE(view.GetRenderTick());
	for (Time tick = 1000; tick < 1100; ++tick)
	{
		ASSERT_TRUE(host.Tick(tick));
		net.Step(20);
	}
	ASSERT_TRUE(view.GetRenderTick());
	EXPECT_NEAR(*view.GetRenderTick(), 1096, 5);
}

TEST(ReplicationClock, PeerSessionsKeepIndependentTimelines)
{
	SimulatedNetwork net;
	auto *viewer = net.AddPeer(2), *first = net.AddPeer(1, 1), *second = net.AddPeer(1, 1);
	for (auto *peer : {viewer, first, second})
	{
		auto &replication = peer->GetReplication();
		ASSERT_TRUE(replication.SetRole(ReplicationRole::Peer));
		ASSERT_TRUE(replication.RegisterReplica<Timed>(ReplicaTypeId{1}, 1));
		replication.SetTickMode(ReplicationTickMode::Manual);
	}
	ASSERT_TRUE(net.Connect(viewer, first));
	ASSERT_TRUE(net.Connect(viewer, second));
	for (Time tick = 1; tick <= 100; ++tick)
	{
		ASSERT_TRUE(first->GetReplication().Tick(tick));
		ASSERT_TRUE(second->GetReplication().Tick(1000 + 2 * tick));
		net.Step(20);
	}
	auto &view = viewer->GetReplication();
	const auto a = view.GetRenderTick(ToPeerGuid(first->GetMyGUID()));
	const auto b = view.GetRenderTick(ToPeerGuid(second->GetMyGUID()));
	EXPECT_FALSE(view.GetRenderTick());
	ASSERT_TRUE(a);
	ASSERT_TRUE(b);
	EXPECT_NEAR(*a, 98, 2);
	EXPECT_NEAR(*b, 1198, 4);
}

TEST(ReplicationClock, UntimedSchemasSendNoClockTraffic)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	auto &host = server->GetReplication(), &view = client->GetReplication();
	ASSERT_TRUE(host.SetRole(ReplicationRole::Server));
	ASSERT_TRUE(view.SetRole(ReplicationRole::Client));
	for (auto *r : {&host, &view})
		ASSERT_TRUE(r->RegisterReplica<Plain>(ReplicaTypeId{1}, 1));
	ASSERT_TRUE(net.Connect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return view.GetConnectionCount() == 1; }, 5000));
	for (unsigned i = 0; i < 30; ++i)
		net.Step(20);
	EXPECT_FALSE(view.GetRenderTick());
	EXPECT_EQ(host.GetStatistics().clockBytes, 0u);
}
}
