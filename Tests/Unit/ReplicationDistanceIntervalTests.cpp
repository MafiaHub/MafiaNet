// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "SimulatedNetwork.h"
#include "mafianet/snapshot_replica.h"
#include <memory>
#include <vector>

using namespace MafiaNet;
namespace
{
struct Counter
{
	uint32_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
// Counts the state records a client applies, so a test sees what the server actually sent.
struct Beacon : SnapshotReplica<Counter>
{
	unsigned received = 0;
	void Deserialize(DeserializeParameters *parameters) override
	{
		if (parameters->bitstreamWrittenTo[0])
			++received;
		SnapshotReplica<Counter>::Deserialize(parameters);
	}
};
constexpr ReplicaTypeId beaconType{31};

// One client observing at (500, 500) with enter radius 100 and leave radius 120, beacons placed by their distance
// along x. Every pass changes every beacon's state, so each one has state to send whenever it is considered.
class ReplicationDistanceIntervals : public ::testing::Test
{
protected:
	std::vector<std::unique_ptr<Beacon>> beacons;
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1), *client = net.AddPeer(1);
	ReplicationManager &host = server->GetReplication();
	PeerGuid observer = UNASSIGNED_PEER_GUID;
	Time tick = 0;
	void SetUp() override
	{
		for (auto *peer : {server, client})
		{
			auto &r = peer->GetReplication();
			ASSERT_TRUE(r.SetRole(peer == server ? ReplicationRole::Server : ReplicationRole::Client));
			ASSERT_TRUE(r.RegisterReplica<Beacon>(beaconType, 1));
		}
		host.SetTickMode(ReplicationTickMode::Manual);
		host.SetSerializeInterval(0);
		ASSERT_TRUE(net.Connect(client, server));
		observer = ToPeerGuid(client->GetMyGUID());
		ASSERT_TRUE(net.RunUntil([&]
		                         { return host.GetPeerStatus(observer) == ReplicationPeerStatus::Active; }, 3000));
		auto &grid = host.GetInterestGrid();
		ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
		ASSERT_TRUE(grid.SetObserverRadius(observer, {500, 500}, 100, 120));
	}
	void TearDown() override
	{
		for (auto &beacon : beacons)
			host.Dereference(beacon.get());
	}
	Beacon &Add(SpatialPoint position)
	{
		beacons.push_back(std::make_unique<Beacon>());
		EXPECT_TRUE(host.Reference(beacons.back().get(), SpatialScope{position}));
		return *beacons.back();
	}
	Beacon &AddAt(float distance) { return Add({500 + distance, 500}); }
	Beacon *Remote(Beacon &beacon) { return static_cast<Beacon *>(client->GetReplication().FindReplica(beacon.GetNetworkID())); }
	void Pass()
	{
		for (auto &beacon : beacons)
			++beacon->state.value;
		ASSERT_TRUE(host.Tick(++tick));
		net.Step(16);
	}
	// State records each beacon's copy applied over the given passes, after every beacon is constructed and settled.
	std::vector<unsigned> SendsOver(unsigned passes)
	{
		for (unsigned i = 0; i < 10; ++i)
			Pass();
		net.Step(200);
		std::vector<unsigned> start;
		for (auto &beacon : beacons)
		{
			auto *remote = Remote(*beacon);
			EXPECT_NE(remote, nullptr);
			start.push_back(remote ? remote->received : 0);
		}
		for (unsigned i = 0; i < passes; ++i)
			Pass();
		net.Step(200);
		std::vector<unsigned> sends;
		for (size_t i = 0; i < beacons.size(); ++i)
		{
			auto *remote = Remote(*beacons[i]);
			sends.push_back(remote ? remote->received - start[i] : 0);
		}
		return sends;
	}
};
}

TEST_F(ReplicationDistanceIntervals, WithoutBandsEveryVisibleObjectIsServedEveryPass)
{
	AddAt(30);
	AddAt(60);
	AddAt(90);
	EXPECT_EQ(SendsOver(12), (std::vector<unsigned>{12, 12, 12}));
	EXPECT_EQ(host.GetStatistics(observer).notDueState, 0u);
}

TEST_F(ReplicationDistanceIntervals, BandsServeFartherObjectsLessOften)
{
	ASSERT_TRUE(host.GetInterestGrid().SetDistanceIntervals({{0.5f, 2}, {0.8f, 3}}));
	AddAt(30);
	AddAt(60);
	AddAt(90);
	EXPECT_EQ(SendsOver(12), (std::vector<unsigned>{12, 6, 4}));
	// On any pass some of the farther objects are not due.
	Pass();
	EXPECT_GT(host.GetStatistics(observer).notDueState, 0u);
	EXPECT_LE(host.GetStatistics(observer).notDueState, 2u);
}

TEST_F(ReplicationDistanceIntervals, AnExplicitUpdateIntervalOverridesTheBand)
{
	ASSERT_TRUE(host.GetInterestGrid().SetDistanceIntervals({{0.5f, 2}, {0.8f, 3}}));
	auto &nearest = AddAt(30);
	auto &farthest = AddAt(90);
	auto &middle = AddAt(60);
	ASSERT_TRUE(host.SetUpdateInterval(observer, nearest, 4));
	ASSERT_TRUE(host.SetUpdateInterval(observer, farthest, 2));
	// 1 clears the explicit interval: the band applies again.
	ASSERT_TRUE(host.SetUpdateInterval(observer, middle, 5));
	ASSERT_TRUE(host.SetUpdateInterval(observer, middle, 1));
	EXPECT_EQ(SendsOver(12), (std::vector<unsigned>{3, 6, 6}));
}

TEST_F(ReplicationDistanceIntervals, ADependencyChildFollowsItsParent)
{
	ASSERT_TRUE(host.GetInterestGrid().SetDistanceIntervals({{0.5f, 2}, {0.8f, 3}}));
	auto &parent = AddAt(90);
	// Far out of range: seen only through the link.
	auto &child = Add({100, 100});
	ASSERT_TRUE(host.GetInterestGrid().SetDependency(child.GetNetworkID(), parent.GetNetworkID()));
	EXPECT_EQ(SendsOver(12), (std::vector<unsigned>{4, 4}));
	EXPECT_EQ(host.GetInterestGrid().GetUpdateInterval(observer, child.GetNetworkID()), 3u);
}

TEST_F(ReplicationDistanceIntervals, ANewlyVisibleObjectIsConstructedAndServedOnItsFirstPass)
{
	ASSERT_TRUE(host.GetInterestGrid().SetDistanceIntervals({{0.5f, 2}, {0.8f, 3}}));
	AddAt(90);
	SendsOver(3);
	// Out of range, then moved into the farthest band: constructed at the next pass with its current state.
	auto &late = Add({100, 100});
	Pass();
	net.Step(200);
	ASSERT_EQ(Remote(late), nullptr);
	ASSERT_TRUE(host.GetInterestGrid().SetPosition(late.GetNetworkID(), {590, 500}));
	late.state.value = 1000;
	ASSERT_TRUE(host.Tick(++tick));
	// Its first pass serves it: only the older object can be not due.
	EXPECT_LE(host.GetStatistics(observer).notDueState, 1u);
	net.Step(200);
	ASSERT_NE(Remote(late), nullptr);
	EXPECT_EQ(Remote(late)->state.value, 1000u);
	// After that it follows its band: a change waits for its next turn, at most 3 passes later.
	late.state.value = 2000;
	unsigned passes = 0;
	while (passes < 4 && Remote(late)->state.value != 2000)
	{
		ASSERT_TRUE(host.Tick(++tick));
		net.Step(50);
		++passes;
	}
	EXPECT_EQ(Remote(late)->state.value, 2000u);
	EXPECT_GE(passes, 2u);
	EXPECT_LE(passes, 3u);
}
