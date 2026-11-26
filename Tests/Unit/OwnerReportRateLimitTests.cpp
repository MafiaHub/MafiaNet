// Copyright (c) 2026, MafiaHub. MIT license.

#include <gtest/gtest.h>

#include "mafianet/replication_manager.h"
#include "mafianet/snapshot_replica.h"
#include "SimulatedNetwork.h"

#include <array>

using namespace MafiaNet;

namespace
{
constexpr ReplicaTypeId kSeatType{31};
constexpr ReplicaTypeId kSignType{32};

struct SeatState
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct SeatInput
{
	uint8_t value = 0;
	void Write(BitStream &out) const { out.Write(value); }
	bool Read(BitStream &in) { return in.Read(value); }
};
struct Seat : SnapshotReplica<SeatState>
{
	unsigned accepted = 0;
	bool Apply(PeerGuid, const SeatInput &input)
	{
		++accepted;
		state.value = input.value;
		return true;
	}
};
// Registered without an owner report.
struct Sign : SnapshotReplica<SeatState>
{
};

// A server and one client that holds authority over `seats`.
class OwnerReportRateLimit : public ::testing::Test
{
protected:
	static constexpr size_t kSeats = 2;
	SimulatedNetwork net{91};
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	std::array<Seat, kSeats> seats;
	PeerGuid driver = ToPeerGuid(client->GetMyGUID());

	ReplicationManager &Host() { return server->GetReplication(); }
	Seat *Copy(size_t i) { return static_cast<Seat *>(client->GetReplication().FindReplica(seats[i].GetNetworkID())); }

	void SetUp() override
	{
		for (auto *peer : {server, client})
		{
			auto &replication = peer->GetReplication();
			ASSERT_TRUE(replication.SetRole(peer == server ? ReplicationRole::Server : ReplicationRole::Client));
			ASSERT_TRUE(replication.RegisterReplica<Seat>(kSeatType, 1));
			ASSERT_TRUE(replication.RegisterReplica<Sign>(kSignType, 1));
			ASSERT_TRUE((replication.RegisterOwnerUpdate<Seat, SeatInput>(1, &Seat::Apply)));
		}
		ASSERT_TRUE(Host().GetInterestGrid().Configure({{0, 0}, {100, 100}, 10}));
		for (auto &seat : seats)
			ASSERT_TRUE(Host().Reference(&seat, SpatialScope{{50, 50}}));
		ASSERT_TRUE(net.Connect(client, server));
		ASSERT_TRUE(net.RunUntil([&]
		                         { return Host().GetPeerStatus(driver) == ReplicationPeerStatus::Active; }, 5000));
		for (auto &seat : seats)
			ASSERT_TRUE(Host().SetAuthority(seat, driver));
		ASSERT_TRUE(net.RunUntil([&]
		                         {
			for (size_t i = 0; i < kSeats; ++i)
				if (!Copy(i) || !client->GetReplication().HasLocalAuthority(*Copy(i)))
					return false;
			return true; }, 5000));
	}

	void Send(size_t seat, unsigned count)
	{
		for (unsigned i = 0; i < count; ++i)
			ASSERT_TRUE(client->GetReplication().SendOwnerUpdate(*Copy(seat), SeatInput{static_cast<uint8_t>(i + 1)}));
	}
	uint64_t Dropped() { return Host().GetStatistics(driver).ownerUpdatesDropped; }
};
}

TEST_F(OwnerReportRateLimit, RejectsTypesWithoutReportsAndABurstWithoutARate)
{
	EXPECT_FALSE(Host().SetOwnerUpdateRateLimit(kSignType, 5));
	EXPECT_FALSE(Host().SetOwnerUpdateRateLimit(ReplicaTypeId{99}, 5));
	EXPECT_FALSE(Host().SetOwnerUpdateRateLimit(kSeatType, 0, 5));
	EXPECT_TRUE(Host().SetOwnerUpdateRateLimit(kSeatType, 5, 10));
	EXPECT_TRUE(Host().SetOwnerUpdateRateLimit(kSeatType, 0));
}

TEST_F(OwnerReportRateLimit, UnlimitedByDefault)
{
	Send(0, 40);
	net.Step(100);
	EXPECT_EQ(seats[0].accepted, 40u);
	EXPECT_EQ(Dropped(), 0u);
}

TEST_F(OwnerReportRateLimit, ReportsBeyondTheBudgetNeverReachTheCallback)
{
	ASSERT_TRUE(Host().SetOwnerUpdateRateLimit(kSeatType, 10, 5));
	Send(0, 20);
	net.Step(50);
	// The full burst; 50 ms at 10 per second refills none.
	EXPECT_EQ(seats[0].accepted, 5u);
	EXPECT_EQ(seats[0].state.value, 5u);
	EXPECT_EQ(Dropped(), 15u);

	// Paced at the rate, every report is accepted, and the client's later sequence numbers still apply.
	for (int i = 0; i < 10; ++i)
	{
		net.Step(100);
		Send(0, 1);
	}
	net.Step(50);
	EXPECT_EQ(seats[0].accepted, 15u);
	EXPECT_EQ(Dropped(), 15u);
}

TEST_F(OwnerReportRateLimit, EachReplicaHasItsOwnBudget)
{
	ASSERT_TRUE(Host().SetOwnerUpdateRateLimit(kSeatType, 3));
	Send(0, 10);
	Send(1, 2);
	net.Step(50);
	EXPECT_EQ(seats[0].accepted, 3u);
	EXPECT_EQ(seats[1].accepted, 2u);
	EXPECT_EQ(Dropped(), 7u);
}

TEST_F(OwnerReportRateLimit, ANewGrantStartsWithAFullBudget)
{
	ASSERT_TRUE(Host().SetOwnerUpdateRateLimit(kSeatType, 1, 2));
	Send(0, 5);
	net.Step(50);
	EXPECT_EQ(seats[0].accepted, 2u);
	ASSERT_TRUE(Host().CorrectAuthority(seats[0]));
	net.Step(200);
	Send(0, 5);
	net.Step(50);
	EXPECT_EQ(seats[0].accepted, 4u);
	EXPECT_EQ(Dropped(), 6u);
}
