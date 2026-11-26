// Copyright (c) 2026, MafiaHub. MIT license.

#include "DelegationFixture.h"

using namespace MafiaNet;
using namespace DelegationTest;

TEST_F(Delegation, RejectsUntrackedReportlessAndClientReplicas)
{
	DelegationPolicy policy;
	Seat untracked;
	ASSERT_TRUE(Host().Reference(&untracked));
	EXPECT_FALSE(Host().SetDelegation(untracked, &policy));
	Sign sign;
	ASSERT_TRUE(Host().Reference(&sign, SpatialScope{kCenter}));
	EXPECT_FALSE(Host().SetDelegation(sign, &policy));
	EXPECT_TRUE(Host().SetDelegation(seat, &policy));
	EXPECT_TRUE(Host().SetDelegation(seat, nullptr));

	auto &client = clients[0]->GetReplication();
	EXPECT_FALSE(client.SetDelegation(seat, &policy));
	EXPECT_FALSE(client.Suspend(guids[1]));
	EXPECT_FALSE(client.Pin(seat, guids[1]));
	EXPECT_FALSE(client.SetClientCapacity(guids[1], 3));
	Host().Dereference(&sign);
	Host().Dereference(&untracked);
}

TEST_F(Delegation, NearestClientAcquires)
{
	Place(0, East(80));
	Place(1, East(40));
	Delegate(seat);
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	ASSERT_EQ(changes.size(), 1u);
	EXPECT_EQ(changes[0].id, seat.GetNetworkID());
	EXPECT_EQ(changes[0].previous, UNASSIGNED_PEER_GUID);
	EXPECT_EQ(changes[0].current, guids[1]);
	EXPECT_EQ(changes[0].change, DelegationChange::Acquired);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat); }, 3000));
	EXPECT_FALSE(Holds(0, seat));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	const auto stats = Host().GetDelegationStatistics();
	EXPECT_EQ(stats.simulated, 1u);
	EXPECT_EQ(stats.dormant, 0u);
	EXPECT_EQ(stats.handovers, 1u);
	EXPECT_GE(stats.elections, 1u);
}

TEST_F(Delegation, HysteresisKeepsTheIncumbent)
{
	AcquireBy(0);
	// Beyond acquire, inside release: kept, even against a challenger at 120 (120^2 > 0.72 * 130^2).
	Place(0, East(130));
	Place(1, East(120));
	Run(1500);
	EXPECT_EQ(changes.size(), 1u);
	EXPECT_TRUE(Holds(0, seat));

	Place(0, East(150));
	Place(1, East(90));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(changes[1].previous, guids[0]);
	EXPECT_EQ(changes[1].current, guids[1]);
	EXPECT_EQ(changes[1].change, DelegationChange::Migrated);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat) && !Holds(0, seat); }, 3000));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
}

TEST_F(Delegation, NobodyInRangeGoesDormant)
{
	AcquireBy(0);
	Place(0, East(200));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes.back().previous, guids[0]);
	EXPECT_EQ(changes.back().current, UNASSIGNED_PEER_GUID);
	EXPECT_EQ(changes.back().change, DelegationChange::Dormant);
	const auto stats = Host().GetDelegationStatistics();
	EXPECT_EQ(stats.dormant, 1u);
	EXPECT_EQ(stats.simulated, 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return !Holds(0, seat); }, 3000));
}

TEST_F(Delegation, CapacityOverflowsToTheNextClient)
{
	Seat second;
	ASSERT_TRUE(Host().Reference(&second, SpatialScope{{kCenter.x, kCenter.y + 10}}));
	ASSERT_TRUE(Host().SetClientCapacity(guids[0], 1));
	Place(0, East(10));
	Place(1, East(60));
	Delegate(seat);
	Delegate(second);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Host().GetDelegationStatistics().simulated == 2; }, 3000));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return (Holds(0, seat) && Holds(1, second)) || (Holds(1, seat) && Holds(0, second)); }, 3000));

	// Weight counts against capacity: with a default of 4, client 1 (load 1) has no room for a weight of 4 either,
	// so the heavy replica stays dormant until an idle client comes near.
	Host().SetClientCapacity(4);
	Seat heavy;
	ASSERT_TRUE(Host().Reference(&heavy, SpatialScope{{kCenter.x, kCenter.y - 10}}));
	Delegate(heavy, 4);
	Run(1500);
	EXPECT_EQ(Host().GetDelegationStatistics().simulated, 2u);
	EXPECT_EQ(Host().GetDelegationStatistics().dormant, 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
	Place(2, East(90));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Host().GetDelegatedLoad(guids[2]) == 4; }, 3000));
	Host().Dereference(&heavy);
	Host().Dereference(&second);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 0u);
}

TEST_F(Delegation, SuspendReleasesImmediately)
{
	AcquireBy(0);
	Place(1, East(80));
	Host().SetDelegationInterval(100000);
	Host().SetTickMode(ReplicationTickMode::Manual);
	Run(50);
	ASSERT_EQ(changes.size(), 1u);
	ASSERT_TRUE(Host().Suspend(guids[0]));
	EXPECT_TRUE(Host().IsSuspended(guids[0]));
	Host().Tick();
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(changes[1].previous, guids[0]);
	EXPECT_EQ(changes[1].current, guids[1]);
	EXPECT_EQ(changes[1].change, DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);

	// Resumed, it is a candidate again but takes nothing from the incumbent until the ranges say so.
	ASSERT_TRUE(Host().Resume(guids[0]));
	EXPECT_FALSE(Host().IsSuspended(guids[0]));
	EXPECT_FALSE(Host().Resume(guids[0]));
}

TEST_F(Delegation, PinOverridesRanges)
{
	AcquireBy(0);
	// Client 2 is 900 away: only a pin gives it the seat.
	ASSERT_TRUE(Host().Pin(seat, guids[2]));
	EXPECT_TRUE(Host().IsPinned(seat));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes.back().previous, guids[0]);
	EXPECT_EQ(changes.back().current, guids[2]);
	EXPECT_EQ(changes.back().change, DelegationChange::Migrated);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(2, seat) && !Holds(0, seat); }, 3000));
	Run(1500);
	EXPECT_EQ(changes.size(), 2u);

	ASSERT_TRUE(Host().Pin(seat, UNASSIGNED_PEER_GUID));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes.back().current, UNASSIGNED_PEER_GUID);
	EXPECT_EQ(changes.back().change, DelegationChange::Dormant);
	EXPECT_EQ(Host().GetDelegationStatistics().dormant, 1u);
	Run(1500);
	EXPECT_EQ(changes.size(), 3u);

	ASSERT_TRUE(Host().Unpin(seat));
	EXPECT_FALSE(Host().IsPinned(seat));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes.back().current, guids[0]);
	EXPECT_EQ(changes.back().change, DelegationChange::Acquired);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(0, seat); }, 3000));
}

TEST_F(Delegation, IneligibleHolderIsReelected)
{
	AcquireBy(0);
	Place(1, East(90));
	ASSERT_TRUE(Host().SetObserverVirtualWorld(guids[0], 5));
	Host().SetDelegationInterval(100000);
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().Tick();
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(changes[1].previous, guids[0]);
	EXPECT_EQ(changes[1].current, guids[1]);
	EXPECT_EQ(changes[1].change, DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
	Host().SetTickMode(ReplicationTickMode::Automatic);
	Host().SetDelegationInterval(100);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat) && !Holds(0, seat); }, 3000));

	// The old holder is nearest, but never granted again while it cannot see the seat.
	Place(1, East(130));
	Place(0, East(5));
	Run(1000);
	EXPECT_EQ(changes.size(), 2u);
	EXPECT_FALSE(Holds(0, seat));
	EXPECT_TRUE(Holds(1, seat));
}

TEST_F(Delegation, OptOutKeepsTheHolder)
{
	AcquireBy(0);
	ASSERT_TRUE(Host().SetDelegation(seat, nullptr));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	Place(0, East(400));
	Place(1, East(10));
	Run(1500);
	EXPECT_EQ(changes.size(), 1u);
	EXPECT_TRUE(Holds(0, seat));
	EXPECT_FALSE(Holds(1, seat));
	const auto stats = Host().GetDelegationStatistics();
	EXPECT_EQ(stats.simulated + stats.dormant, 0u);
}

TEST_F(Delegation, DisconnectReelectsAtOnce)
{
	AcquireBy(0);
	Place(1, East(80));
	Host().SetDelegationInterval(100000);
	Run(50);
	clients[0]->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes.back().previous, guids[0]);
	EXPECT_EQ(changes.back().current, guids[1]);
	EXPECT_EQ(changes.back().change, DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat); }, 3000));
}

TEST_F(Delegation, PassRequestedFromACallbackWaitsForTheNextTick)
{
	Place(0, East(50));
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().SetDelegationInterval(0);
	bool pinned = false;
	Host().SetDelegationCallback([&](NetworkID id, PeerGuid previous, PeerGuid current, DelegationChange change)
	                             {
		changes.push_back({id, previous, current, change});
		if (!pinned)
		{
			pinned = true;
			EXPECT_TRUE(Host().Pin(seat, guids[2]));
		} });
	Delegate(seat);
	Host().Tick();
	ASSERT_EQ(changes.size(), 1u);
	EXPECT_EQ(changes[0].current, guids[0]);
	Host().Tick();
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(changes[1].current, guids[2]);
}

TEST_F(Delegation, PassIsStaggered)
{
	constexpr size_t kSeats = 100;
	std::vector<std::unique_ptr<Seat>> seats;
	for (size_t i = 0; i < kSeats; ++i)
	{
		seats.push_back(std::make_unique<Seat>());
		ASSERT_TRUE(Host().Reference(seats.back().get(), SpatialScope{{static_cast<float>(10 + i * 9), 900}}));
		Delegate(*seats.back());
	}
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().SetDelegationInterval(500);
	// 30 Hz: 15 ticks of 33 1/3 ms are one 500 ms interval.
	std::vector<uint64_t> totals;
	uint32_t elapsedMs = 0;
	for (uint32_t tick = 0; tick < 150; ++tick)
	{
		const uint32_t next = (tick + 1) * 100 / 3;
		net.Step(next - elapsedMs);
		elapsedMs = next;
		Host().Tick();
		totals.push_back(Host().GetDelegationStatistics().elections);
	}
	for (size_t i = 1; i < totals.size(); ++i)
		EXPECT_LT(totals[i] - totals[i - 1], 30u) << "tick " << i;
	for (size_t i = 30; i + 15 < totals.size(); i += 15)
	{
		const uint64_t perInterval = totals[i + 15] - totals[i];
		EXPECT_GE(perInterval, 90u) << "window at tick " << i;
		EXPECT_LE(perInterval, 110u) << "window at tick " << i;
	}
	for (auto &replica : seats)
		Host().Dereference(replica.get());
}

TEST_F(Delegation, ManualAuthorityChangesKeepLoadsAndAreReelected)
{
	AcquireBy(0);
	Host().SetDelegationInterval(100000);
	Host().SetTickMode(ReplicationTickMode::Manual);
	// Revoked by hand: the load follows the grant at once, and the next Tick re-elects the same client.
	ASSERT_TRUE(Host().RevokeAuthority(seat));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	EXPECT_EQ(Host().GetDelegationStatistics().dormant, 1u);
	Host().Tick();
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 1u);
	// Granted by hand to a client out of range: charged to it, then overridden by the next pass.
	ASSERT_TRUE(Host().SetAuthority(seat, guids[2]));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 1u);
	Host().Tick();
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 0u);
	// The game was never told of a change that the pass undid.
	EXPECT_EQ(changes.size(), 1u);
	// A replica that goes away refunds its holder.
	Host().SetTickMode(ReplicationTickMode::Automatic);
	Host().Dereference(&seat);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	EXPECT_EQ(Host().GetDelegationStatistics().simulated + Host().GetDelegationStatistics().dormant, 0u);
}

TEST_F(Delegation, PinWaitsOutASuspensionOfItsClient)
{
	AcquireBy(0);
	ASSERT_TRUE(Host().Pin(seat, guids[2]));
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.current == guids[2]; }));
	ASSERT_TRUE(Host().Suspend(guids[2]));
	ASSERT_TRUE(AwaitChange([](const Change &c)
	                        { return c.change == DelegationChange::Dormant; }));
	EXPECT_EQ(changes.back().previous, guids[2]);
	// Pinned: client 0, near and free, is not elected while the pin waits.
	Run(1500);
	EXPECT_EQ(changes.back().change, DelegationChange::Dormant);
	EXPECT_EQ(Host().GetDelegationStatistics().dormant, 1u);
	EXPECT_TRUE(Host().IsPinned(seat));

	ASSERT_TRUE(Host().Resume(guids[2]));
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.change == DelegationChange::Acquired && c.current == guids[2]; }));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(2, seat); }, 3000));
}

TEST_F(Delegation, PinEndsWhenItsClientDisconnects)
{
	AcquireBy(0);
	ASSERT_TRUE(Host().Pin(seat, guids[2]));
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.current == guids[2]; }));
	clients[2]->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.previous == guids[2]; }));
	EXPECT_FALSE(Host().IsPinned(seat));
	// The normal election runs at once: client 0 is in range.
	EXPECT_EQ(changes.back().current, guids[0]);
	EXPECT_EQ(changes.back().change, DelegationChange::Migrated);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(0, seat); }, 3000));
}

TEST_F(Delegation, UnknownClientsAreRefused)
{
	Delegate(seat);
	const PeerGuid stranger{12345};
	EXPECT_FALSE(Host().Pin(seat, stranger));
	EXPECT_FALSE(Host().IsPinned(seat));
	EXPECT_FALSE(Host().Suspend(stranger));
	EXPECT_FALSE(Host().IsSuspended(stranger));
	EXPECT_FALSE(Host().Resume(stranger));
	EXPECT_FALSE(Host().SetClientCapacity(stranger, 2));
	EXPECT_FALSE(Host().Suspend(UNASSIGNED_PEER_GUID));
	// A replica that is not delegated cannot be pinned, even to a known client.
	ASSERT_TRUE(Host().SetDelegation(seat, nullptr));
	EXPECT_FALSE(Host().Pin(seat, guids[0]));
	EXPECT_FALSE(Host().Unpin(seat));
}

TEST_F(Delegation, DeclinedClientIsPassedOver)
{
	// Client 0 is nearest, but 70^2 > 0.72 * 80^2: once client 1 holds the seat, client 0 cannot take it back.
	DeclineFromAfar(0);
	Place(0, East(70));
	Place(1, East(80));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes[0].current, guids[1]);
	EXPECT_EQ(changes[0].change, DelegationChange::Acquired);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat); }, 3000));
	EXPECT_FALSE(Holds(0, seat));

	// Withdrawn: the incumbent is inside release and the sticky rule keeps it.
	Decline(0, false);
	Run(1500);
	EXPECT_EQ(changes.size(), 1u);
	EXPECT_TRUE(Holds(1, seat));

	// Two decline changes within 100 ms run one extra pass, not two.
	Place(2, East(120));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(2, seat) != nullptr; }, 3000));
	Host().SetDelegationInterval(100000);
	Host().SetTickMode(ReplicationTickMode::Manual);
	Run(20);
	Host().Tick();
	const uint64_t before = Host().GetDelegationStatistics().elections;
	ASSERT_TRUE(clients[0]->GetReplication().DeclineAuthority(*Copy(0, seat), true));
	Run(20);
	Host().Tick();
	EXPECT_EQ(Host().GetDelegationStatistics().elections, before + 1);
	ASSERT_TRUE(clients[2]->GetReplication().DeclineAuthority(*Copy(2, seat), true));
	for (int tick = 0; tick < 6; ++tick)
	{
		Run(10);
		Host().Tick();
	}
	EXPECT_EQ(Host().GetDelegationStatistics().declines, 3u);
	EXPECT_EQ(Host().GetDelegationStatistics().elections, before + 1);
	// The second change is not lost: it runs once the gap has passed.
	for (int tick = 0; tick < 10; ++tick)
	{
		Run(10);
		Host().Tick();
	}
	EXPECT_EQ(Host().GetDelegationStatistics().elections, before + 2);
	EXPECT_EQ(changes.size(), 1u);
}

TEST_F(Delegation, DeclineClearsWhenTheReplicaLeavesScope)
{
	DeclineFromAfar(0);
	Place(0, East(50));
	Run(1500);
	EXPECT_TRUE(changes.empty());
	EXPECT_EQ(Host().GetDelegationStatistics().dormant, 1u);

	// Out of its visible set, then back: the decline is forgotten.
	Place(0, {100, 100});
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Copy(0, seat) == nullptr; }, 3000));
	Place(0, East(50));
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.change == DelegationChange::Acquired && c.current == guids[0]; }));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(0, seat); }, 3000));
}

TEST_F(Delegation, PinOverridesDecline)
{
	DeclineFromAfar(0);
	Place(0, East(50));
	Run(1500);
	EXPECT_TRUE(changes.empty());

	ASSERT_TRUE(Host().Pin(seat, guids[0]));
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.change == DelegationChange::Acquired && c.current == guids[0]; }));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(0, seat); }, 3000));

	// Unpinned, the decline is still in force.
	ASSERT_TRUE(Host().Unpin(seat));
	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_EQ(changes.back().previous, guids[0]);
	EXPECT_EQ(changes.back().current, UNASSIGNED_PEER_GUID);
	EXPECT_EQ(changes.back().change, DelegationChange::Dormant);
}

TEST_F(Delegation, VetoPostponesThenForces)
{
	AcquireBy(0);
	Delegate(seat, 1, 1000);
	std::vector<std::pair<NetworkID, PeerGuid>> asked;
	Host().SetReleaseVeto([&](NetworkID id, PeerGuid holder)
	                      {
		asked.emplace_back(id, holder);
		return false; });
	Host().SetDelegationInterval(100);
	Place(0, East(150));
	Place(1, East(50));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Host().GetDelegationStatistics().refusals > 0; }, 3000));
	const TimeUS firstRefusal = net.Now();
	ASSERT_FALSE(asked.empty());
	EXPECT_EQ(asked.front().first, seat.GetNetworkID());
	EXPECT_EQ(asked.front().second, guids[0]);
	Run(500);
	EXPECT_EQ(changes.size(), 1u);
	EXPECT_TRUE(Holds(0, seat));
	EXPECT_GE(Host().GetDelegationStatistics().refusals, 3u);
	EXPECT_EQ(Host().GetDelegationStatistics().forced, 0u);

	ASSERT_TRUE(AwaitChange([](const Change &)
	                        { return true; }));
	EXPECT_GE(net.Now() - firstRefusal, 900000u);
	// Forced at the first pass after the grace: graceMs, plus at most one interval, plus a tick of slack.
	EXPECT_LE(net.Now() - firstRefusal, 1000000u + 100000u + 50000u);
	EXPECT_EQ(changes.back().previous, guids[0]);
	EXPECT_EQ(changes.back().current, guids[1]);
	EXPECT_EQ(changes.back().change, DelegationChange::Migrated);
	const auto stats = Host().GetDelegationStatistics();
	EXPECT_EQ(stats.forced, 1u);
	EXPECT_EQ(stats.handovers, 2u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat) && !Holds(0, seat); }, 3000));
}

TEST_F(Delegation, DisconnectIgnoresTheVeto)
{
	AcquireBy(0);
	Host().SetReleaseVeto([](NetworkID, PeerGuid)
	                      { return false; });
	Host().SetDelegationInterval(100);
	Place(0, East(150));
	Place(1, East(50));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Host().GetDelegationStatistics().refusals > 0; }, 3000));
	Host().SetDelegationInterval(100000);
	Host().SetTickMode(ReplicationTickMode::Manual);
	clients[0]->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Host().GetPeerStatus(guids[0]) != ReplicationPeerStatus::Active; }, 3000));
	// Re-elected by the disconnect itself, before any Tick, and the callback follows the next one.
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
	Host().Tick();
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(changes[1].previous, guids[0]);
	EXPECT_EQ(changes[1].current, guids[1]);
	EXPECT_EQ(changes[1].change, DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegationStatistics().forced, 0u);
}

TEST_F(Delegation, MalformedDeclinesAreDropped)
{
	using namespace ReplicationWire;
	DeclineFromAfar(0);
	Place(0, East(50));
	// Only a client may decline, and only a replica it has.
	EXPECT_FALSE(Host().DeclineAuthority(seat, true));
	Seat stranger;
	EXPECT_FALSE(clients[0]->GetReplication().DeclineAuthority(stranger, true));
	const Header session = static_cast<RecordingPeer *>(clients[0])->decline;
	ASSERT_EQ(session.kind, Kind::Decline);
	auto forge = [&](NetworkID id, bool withFlag, uint8_t flag, bool trailing)
	{
		auto out = std::make_unique<BitStream>();
		ReplicationWire::WriteHeader(*out, Kind::Decline, session.clientToken, session.serverToken);
		out->Write(id);
		if (withFlag)
			out->Write(flag);
		if (trailing)
			out->Write(static_cast<uint8_t>(7));
		return out;
	};
	const NetworkID id = seat.GetNetworkID();
	std::vector<std::unique_ptr<BitStream>> malformed;
	malformed.push_back(forge(id, false, 0, false));
	malformed.push_back(forge(id, true, 2, false));
	malformed.push_back(forge(id, true, 0, true));
	malformed.push_back(forge(id + 1000, true, 0, false));
	malformed.push_back(forge(UNASSIGNED_NETWORK_ID, true, 0, false));
	malformed.push_back(std::make_unique<BitStream>());
	malformed.back()->Write(static_cast<MessageID>(ID_REPLICATION));
	malformed.back()->Write(Version);
	malformed.back()->Write(Kind::Decline);
	for (auto &packet : malformed)
		clients[0]->Send(packet.get(), Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	// A server-role peer sending one to a client: the client drops it.
	const auto fromServer = forge(id, true, 0, false);
	server->Send(fromServer.get(), Priority::High, Reliability::ReliableOrdered, 0, clients[0]->GetMyGUID(), false);
	Run(500);
	// None was applied: the decline still stands, and only the real one was counted.
	EXPECT_EQ(Host().GetDelegationStatistics().declines, 1u);
	EXPECT_TRUE(changes.empty());
	// The session still works: a well-formed withdrawal lets the client acquire.
	ASSERT_TRUE(clients[0]->GetReplication().DeclineAuthority(*Copy(0, seat), false));
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.change == DelegationChange::Acquired && c.current == guids[0]; }));
}

TEST_F(Delegation, HolderDecliningIsReleasedWithoutTheVeto)
{
	AcquireBy(0);
	Place(1, East(80));
	unsigned asked = 0;
	Host().SetReleaseVeto([&](NetworkID, PeerGuid)
	                      {
		++asked;
		return false; });
	Run(300);
	ASSERT_EQ(changes.size(), 1u);
	// The holder gives the seat up itself: the next decline pass moves it on, and the veto is not consulted.
	Decline(0, true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() == 2; }, 3000));
	EXPECT_EQ(changes.back().previous, guids[0]);
	EXPECT_EQ(changes.back().current, guids[1]);
	EXPECT_EQ(changes.back().change, DelegationChange::Migrated);
	EXPECT_EQ(asked, 0u);
	const auto stats = Host().GetDelegationStatistics();
	EXPECT_EQ(stats.refusals, 0u);
	EXPECT_EQ(stats.forced, 0u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Holds(1, seat) && !Holds(0, seat); }, 3000));
}

TEST_F(Delegation, DeclineForAReplicaTheSenderDoesNotHaveIsDropped)
{
	using namespace ReplicationWire;
	// Delegated and on the server, but nowhere near client 0: it was never constructed for it.
	Seat far;
	ASSERT_TRUE(Host().Reference(&far, SpatialScope{{900, 900}}));
	Delegate(far);
	DeclineFromAfar(0);
	ASSERT_EQ(Host().GetDelegationStatistics().declines, 1u);
	const Header session = static_cast<RecordingPeer *>(clients[0])->decline;
	ASSERT_EQ(session.kind, Kind::Decline);
	BitStream forged;
	WriteHeader(forged, Kind::Decline, session.clientToken, session.serverToken);
	ReplicationWire::Decline{far.GetNetworkID(), true}.Write(forged);
	clients[0]->Send(&forged, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false);
	Run(500);
	EXPECT_EQ(Host().GetDelegationStatistics().declines, 1u);
	// Not recorded: client 0 acquires the far seat once it comes near.
	Place(0, {880, 900});
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.id == far.GetNetworkID() && c.current == guids[0]; }));
	Host().Dereference(&far);
}

TEST_F(Delegation, DisconnectReelectsOnlyWhatTheClientHeld)
{
	AcquireBy(0);
	// A second seat held by client 1, pinned away from it: that handover waits for the next Tick and its veto.
	Seat other;
	ASSERT_TRUE(Host().Reference(&other, SpatialScope{{kCenter.x, kCenter.y + 300}}));
	Place(1, {kCenter.x + 50, kCenter.y + 300});
	Delegate(other);
	ASSERT_TRUE(AwaitChange([&](const Change &c)
	                        { return c.id == other.GetNetworkID() && c.current == guids[1]; }));
	std::vector<NetworkID> asked;
	Host().SetReleaseVeto([&](NetworkID id, PeerGuid)
	                      {
		asked.push_back(id);
		return true; });
	Host().SetDelegationInterval(100000);
	Host().SetTickMode(ReplicationTickMode::Manual);
	ASSERT_TRUE(Host().Pin(other, guids[2]));
	clients[0]->CloseConnection(server->GetMyGUID(), true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Host().GetPeerStatus(guids[0]) != ReplicationPeerStatus::Active; }, 3000));
	// The disconnect re-elected the seat client 0 held (nobody is near: dormant) and nothing else.
	EXPECT_TRUE(asked.empty());
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 0u);
	Host().Tick();
	ASSERT_EQ(asked.size(), 1u);
	EXPECT_EQ(asked[0], other.GetNetworkID());
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 1u);
	Host().Dereference(&other);
}
