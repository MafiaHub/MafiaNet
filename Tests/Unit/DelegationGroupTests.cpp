// Copyright (c) 2026, MafiaHub. MIT license.

#include "DelegationFixture.h"

#include <set>

using namespace MafiaNet;
using namespace DelegationTest;

namespace
{
// The seat leads two members placed next to it.
class DelegationGroups : public Delegation
{
protected:
	Seat first, second;

	void SetUp() override
	{
		Delegation::SetUp();
		ASSERT_TRUE(Host().Reference(&first, SpatialScope{{kCenter.x, kCenter.y + 20}}));
		ASSERT_TRUE(Host().Reference(&second, SpatialScope{{kCenter.x, kCenter.y - 20}}));
	}
	void Link()
	{
		ASSERT_TRUE(Host().SetDelegationGroup(first, &seat));
		ASSERT_TRUE(Host().SetDelegationGroup(second, &seat));
	}
	std::set<NetworkID> Ids(size_t from) const
	{
		std::set<NetworkID> out;
		for (size_t i = from; i < changes.size(); ++i)
			out.insert(changes[i].id);
		return out;
	}
	std::set<NetworkID> All() { return {seat.GetNetworkID(), first.GetNetworkID(), second.GetNetworkID()}; }
	// The changes since \a from all move to \a current, as \a change.
	void ExpectAllMoved(size_t from, PeerGuid current, DelegationChange change)
	{
		ASSERT_EQ(changes.size(), from + 3);
		for (size_t i = from; i < changes.size(); ++i)
		{
			EXPECT_EQ(changes[i].current, current);
			EXPECT_EQ(changes[i].change, change);
		}
		EXPECT_EQ(Ids(from), All());
	}
	bool HoldsAll(size_t client) { return Holds(client, seat) && Holds(client, first) && Holds(client, second); }
	bool HoldsNone(size_t client) { return !Holds(client, seat) && !Holds(client, first) && !Holds(client, second); }
};
} // namespace

TEST_F(DelegationGroups, MembersFollowAnElectedLeader)
{
	Link();
	// Client 0 is nearest, but has room for 5 and the group weighs 2 + 3 + 1.
	ASSERT_TRUE(Host().SetClientCapacity(guids[0], 5));
	Place(0, East(10));
	Place(1, East(90));
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().SetDelegationInterval(0);
	Delegate(seat, 2);
	// A member's own policy is ignored while grouped, all but its weight: alone, it could never be acquired.
	DelegationPolicy own;
	own.acquireRange = 0;
	own.releaseRange = 0;
	own.weight = 3;
	ASSERT_TRUE(Host().SetDelegation(first, &own));
	Host().Tick();
	ExpectAllMoved(0, guids[1], DelegationChange::Acquired);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 6u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
	const auto stats = Host().GetDelegationStatistics();
	// One election for the whole group; one handover per grant.
	EXPECT_EQ(stats.elections, 1u);
	EXPECT_EQ(stats.handovers, 3u);
	EXPECT_EQ(stats.simulated, 3u);
	Host().SetTickMode(ReplicationTickMode::Automatic);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return HoldsAll(1); }, 3000));

	// The leader's position decides: moving it out of client 1's release range moves the group to client 2, members
	// and all, though the members stay where they were.
	ASSERT_TRUE(Host().SetClientCapacity(guids[0], 0));
	const size_t before = changes.size();
	ASSERT_TRUE(Grid().SetPosition(seat.GetNetworkID(), {kCenter.x, kCenter.y + 400}));
	Place(2, {kCenter.x + 30, kCenter.y + 400});
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() >= before + 3; }, 3000));
	ExpectAllMoved(before, guids[2], DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 6u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return HoldsAll(2) && HoldsNone(1); }, 3000));
}

TEST_F(DelegationGroups, MembersFollowAHandOwnedLeader)
{
	Link();
	// Client 2 sees the seat but is far beyond any acquire range: only the game's own grant puts it there.
	Place(2, East(150));
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().SetDelegationInterval(100000);
	ASSERT_TRUE(Host().SetAuthority(seat, guids[2]));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 1u);
	Host().Tick();
	// The members follow at the next pass. The game made the leader's change, so it is not reported.
	ASSERT_EQ(changes.size(), 2u);
	EXPECT_EQ(Ids(0), (std::set<NetworkID>{first.GetNetworkID(), second.GetNetworkID()}));
	for (const auto &c : changes)
	{
		EXPECT_EQ(c.current, guids[2]);
		EXPECT_EQ(c.change, DelegationChange::Acquired);
	}
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 3u);
	Host().SetTickMode(ReplicationTickMode::Automatic);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return HoldsAll(2); }, 3000));
	// A member granted elsewhere by hand is put back with its leader.
	Host().SetTickMode(ReplicationTickMode::Manual);
	ASSERT_TRUE(Host().SetAuthority(first, guids[1]));
	Host().Tick();
	EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 0u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 3u);
	EXPECT_EQ(changes.size(), 2u);

	// Clearing the leader's authority drops theirs.
	ASSERT_TRUE(Host().RevokeAuthority(seat));
	Host().Tick();
	ASSERT_EQ(changes.size(), 4u);
	for (size_t i = 2; i < 4; ++i)
	{
		EXPECT_EQ(changes[i].previous, guids[2]);
		EXPECT_EQ(changes[i].current, UNASSIGNED_PEER_GUID);
		EXPECT_EQ(changes[i].change, DelegationChange::Dormant);
	}
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 0u);
	// A hand-owned group is not the election's: it cannot be pinned.
	EXPECT_FALSE(Host().Pin(first, guids[2]));
	Host().SetTickMode(ReplicationTickMode::Automatic);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return HoldsNone(2); }, 3000));
}

TEST_F(DelegationGroups, IneligibleMemberBlocksTheWholeGroup)
{
	// The first member is in dimension 7. Client 0 (nearest, default dimension) cannot see it; client 1 sees every
	// dimension.
	SpatialScope hidden{{kCenter.x, kCenter.y + 20}};
	hidden.dimension = 7;
	ASSERT_TRUE(Grid().SetScope(first.GetNetworkID(), hidden));
	ASSERT_TRUE(Host().SetObserverVirtualWorld(guids[1], VIRTUAL_WORLD_GLOBAL));
	Link();
	Place(0, East(10));
	Place(1, East(90));
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().SetDelegationInterval(0);
	Delegate(seat);
	for (int tick = 0; tick < 10; ++tick)
	{
		Host().Tick();
		EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u) << "tick " << tick;
		EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 3u) << "tick " << tick;
	}
	ExpectAllMoved(0, guids[1], DelegationChange::Acquired);

	// Client 1 now sees only dimension 7: MafiaNet revokes the seat and the second member from it, and the pass
	// releases the first member in the same Tick. Nobody is eligible for all three, so the whole group is dormant.
	ASSERT_TRUE(Host().SetObserverVirtualWorld(guids[1], 7));
	for (int tick = 0; tick < 10; ++tick)
	{
		Host().Tick();
		EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u) << "tick " << tick;
		EXPECT_EQ(Host().GetDelegatedLoad(guids[1]), 0u) << "tick " << tick;
	}
	ExpectAllMoved(3, UNASSIGNED_PEER_GUID, DelegationChange::Dormant);
	EXPECT_EQ(Host().GetDelegationStatistics().dormant, 3u);
	Host().SetTickMode(ReplicationTickMode::Automatic);
	Run(500);
	EXPECT_FALSE(Holds(1, first));
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);
}

TEST_F(DelegationGroups, PinAndDeclineActOnTheGroup)
{
	Link();
	Place(0, East(50));
	Place(1, East(80));
	Host().SetDelegationInterval(100);
	Delegate(seat);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() == 3; }, 3000));
	ExpectAllMoved(0, guids[0], DelegationChange::Acquired);

	// Pinning a member pins the group.
	ASSERT_TRUE(Host().Pin(second, guids[2]));
	EXPECT_TRUE(Host().IsPinned(seat));
	EXPECT_TRUE(Host().IsPinned(first));
	EXPECT_TRUE(Host().IsPinned(second));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() == 6; }, 3000));
	ExpectAllMoved(3, guids[2], DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 3u);
	ASSERT_TRUE(Host().Unpin(first));
	EXPECT_FALSE(Host().IsPinned(seat));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() == 9; }, 3000));
	ExpectAllMoved(6, guids[0], DelegationChange::Migrated);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return HoldsAll(0); }, 3000));

	// The holder declines one member: the whole group goes to client 1.
	const uint64_t declines = Host().GetDelegationStatistics().declines;
	ASSERT_TRUE(clients[0]->GetReplication().DeclineAuthority(*Copy(0, first), true));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() == 12; }, 3000));
	EXPECT_EQ(Host().GetDelegationStatistics().declines, declines + 1);
	ExpectAllMoved(9, guids[1], DelegationChange::Migrated);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 0u);

	// Suspending the holder releases the group. Client 0 declined a member, client 2 is far: it goes dormant.
	ASSERT_TRUE(Host().Suspend(guids[1]));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return changes.size() == 15; }, 3000));
	ExpectAllMoved(12, UNASSIGNED_PEER_GUID, DelegationChange::Dormant);
	Run(500);
	EXPECT_EQ(changes.size(), 15u);
}

TEST_F(DelegationGroups, ChainsFlattenAndCyclesFail)
{
	ASSERT_TRUE(Host().SetDelegationGroup(first, &seat));
	ASSERT_TRUE(Host().SetDelegationGroup(second, &first));
	EXPECT_FALSE(Host().SetDelegationGroup(seat, &second));
	EXPECT_FALSE(Host().SetDelegationGroup(seat, &seat));
	EXPECT_FALSE(clients[0]->GetReplication().SetDelegationGroup(second, &seat));
	Seat untracked;
	ASSERT_TRUE(Host().Reference(&untracked));
	EXPECT_FALSE(Host().SetDelegationGroup(untracked, &seat));
	EXPECT_FALSE(Host().SetDelegationGroup(second, &untracked));
	// The grid's dependencies follow the links as given.
	EXPECT_EQ(Grid().GetDependency(first.GetNetworkID()), seat.GetNetworkID());
	EXPECT_EQ(Grid().GetDependency(second.GetNetworkID()), first.GetNetworkID());
	EXPECT_EQ(Grid().GetDependency(seat.GetNetworkID()), UNASSIGNED_NETWORK_ID);

	// The chain is one group under its root.
	Place(0, East(50));
	Host().SetTickMode(ReplicationTickMode::Manual);
	Host().SetDelegationInterval(0);
	Delegate(seat);
	Host().Tick();
	ExpectAllMoved(0, guids[0], DelegationChange::Acquired);
	EXPECT_EQ(Host().GetDelegationStatistics().elections, 1u);
	ASSERT_TRUE(Host().Pin(second, guids[1]));
	EXPECT_TRUE(Host().IsPinned(seat));
	Host().Tick();
	ExpectAllMoved(3, guids[1], DelegationChange::Migrated);
	ASSERT_TRUE(Host().Unpin(seat));
	Host().Tick();
	ExpectAllMoved(6, guids[0], DelegationChange::Migrated);

	// The middle leaves. It keeps its holder, and the end now follows it, as a leader the game owns.
	ASSERT_TRUE(Host().SetDelegationGroup(first, nullptr));
	EXPECT_EQ(Grid().GetDependency(first.GetNetworkID()), UNASSIGNED_NETWORK_ID);
	EXPECT_EQ(Grid().GetDependency(second.GetNetworkID()), first.GetNetworkID());
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 3u);
	ASSERT_TRUE(Host().SetAuthority(first, guids[2]));
	Host().Tick();
	EXPECT_EQ(Host().GetDelegatedLoad(guids[0]), 1u);
	EXPECT_EQ(Host().GetDelegatedLoad(guids[2]), 2u);
	ASSERT_EQ(changes.size(), 10u);
	EXPECT_EQ(changes.back().id, second.GetNetworkID());
	EXPECT_EQ(changes.back().current, guids[2]);
	// Leaving when not grouped is a no-op.
	EXPECT_TRUE(Host().SetDelegationGroup(first, nullptr));
	Host().Dereference(&untracked);
}
