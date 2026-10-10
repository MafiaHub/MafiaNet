/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "mafianet/team_manager.h"
#include "mafianet/peer_interface.h"

using namespace MafiaNet;

/*
Description:
Covers the registries TeamManager keeps -- the world list, and per world the team list, team-member
list, participant list and the two NetworkID hash tables -- written before they move to the
standard library (std migration stage 2, #60). The subsystem had no tests at all.

Two orderings here are publicly observable and differ from each other, which is what makes a
mechanical container swap risky:

 - TM_World::GetTeamByIndex / GetTeamIndex and the team-member equivalents publish the index of
   order-PRESERVING lists: DereferenceTeam and DereferenceTeamMember use RemoveAtIndex, so
   removing one shifts the rest down.
 - TeamManager::GetWorldAtIndex publishes the index of an order-NOT-preserving list: RemoveWorld
   uses RemoveAtIndexFast, which swaps the last world into the hole.

A migration that used one idiom for both would quietly renumber half of these, and the NetworkID
lookups (teamsHash, teamMembersHash) would still answer correctly, so nothing else would notice.

Success conditions: index order is ascending-by-insertion for teams and members and survives a
removal in the middle; world removal swaps rather than shifts; NetworkID lookups resolve to the
registered object and report absence otherwise; participants round-trip.
*/

namespace
{
	class TestTeam : public TM_Team
	{
	};

	class TestTeamMember : public TM_TeamMember
	{
	};

	class TeamManagerRegistry : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			peer = RakPeerInterface::GetInstance();
			peer->AttachPlugin(&manager);
			world = manager.AddWorld(0);
			ASSERT_NE(world, (TM_World *) 0);
		}
		void TearDown() override
		{
			peer->DetachPlugin(&manager);
			RakPeerInterface::DestroyInstance(peer);
		}

		std::vector<NetworkID> TeamIdsInIndexOrder(void) const
		{
			std::vector<NetworkID> out;
			for (unsigned i = 0; i < world->GetTeamCount(); ++i)
			{
				TM_Team *t = world->GetTeamByIndex(i);
				if (t == 0)
					break;
				out.push_back(t->GetNetworkID());
			}
			return out;
		}

		RakPeerInterface *peer = nullptr;
		TeamManager manager;
		TM_World *world = nullptr;
	};
}

TEST_F(TeamManagerRegistry, AWorldStartsEmpty)
{
	EXPECT_EQ(world->GetTeamCount(), 0u);
	EXPECT_EQ(world->GetTeamMemberCount(), 0u);
}

TEST_F(TeamManagerRegistry, TeamsAreIndexedInRegistrationOrder)
{
	TestTeam a, b, c;
	world->ReferenceTeam(&a, 10, false);
	world->ReferenceTeam(&b, 20, false);
	world->ReferenceTeam(&c, 30, false);

	ASSERT_EQ(world->GetTeamCount(), 3u);
	std::vector<NetworkID> order = TeamIdsInIndexOrder();
	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], static_cast<NetworkID>(10));
	EXPECT_EQ(order[1], static_cast<NetworkID>(20));
	EXPECT_EQ(order[2], static_cast<NetworkID>(30));

	EXPECT_EQ(world->GetTeamIndex(&a), 0u);
	EXPECT_EQ(world->GetTeamIndex(&b), 1u);
	EXPECT_EQ(world->GetTeamIndex(&c), 2u);
}

// DereferenceTeam uses RemoveAtIndex, so the list shifts; it must NOT swap the last into the hole.
TEST_F(TeamManagerRegistry, DereferencingATeamShiftsTheRestDown)
{
	TestTeam a, b, c;
	world->ReferenceTeam(&a, 10, false);
	world->ReferenceTeam(&b, 20, false);
	world->ReferenceTeam(&c, 30, false);

	world->DereferenceTeam(&a, 0); // index 0, with index 2 being last

	ASSERT_EQ(world->GetTeamCount(), 2u);
	std::vector<NetworkID> order = TeamIdsInIndexOrder();
	ASSERT_EQ(order.size(), 2u);
	EXPECT_EQ(order[0], static_cast<NetworkID>(20)) << "the remainder must shift down, not be swapped";
	EXPECT_EQ(order[1], static_cast<NetworkID>(30));
	EXPECT_EQ(world->GetTeamIndex(&b), 0u);
	EXPECT_EQ(world->GetTeamIndex(&c), 1u);
}

TEST_F(TeamManagerRegistry, TeamsAreFoundByNetworkId)
{
	TestTeam a, b;
	world->ReferenceTeam(&a, 10, false);
	world->ReferenceTeam(&b, 20, false);

	EXPECT_EQ(world->GetTeamByNetworkID(10), &a);
	EXPECT_EQ(world->GetTeamByNetworkID(20), &b);
	EXPECT_EQ(world->GetTeamByNetworkID(999), (TM_Team *) 0);
}

TEST_F(TeamManagerRegistry, ADereferencedTeamIsNoLongerFoundByNetworkId)
{
	TestTeam a;
	world->ReferenceTeam(&a, 10, false);
	ASSERT_EQ(world->GetTeamByNetworkID(10), &a);

	world->DereferenceTeam(&a, 0);

	EXPECT_EQ(world->GetTeamByNetworkID(10), (TM_Team *) 0);
	EXPECT_EQ(world->GetTeamCount(), 0u);
}

TEST_F(TeamManagerRegistry, ManyTeamsAreAllReachableByNetworkId)
{
	// Enough to spread across hash buckets, so a migration that collapses colliding keys shows up.
	const int count = 120;
	std::vector<TestTeam *> teams;
	for (int i = 0; i < count; ++i)
	{
		TestTeam *t = new TestTeam();
		teams.push_back(t);
		world->ReferenceTeam(t, (NetworkID) (1000 + i), false);
	}
	ASSERT_EQ(world->GetTeamCount(), (unsigned) count);
	for (int i = 0; i < count; ++i)
		EXPECT_EQ(world->GetTeamByNetworkID((NetworkID) (1000 + i)), teams[i]) << "team " << i;

	for (int i = 0; i < count; ++i)
	{
		world->DereferenceTeam(teams[i], 0);
		delete teams[i];
	}
}

TEST_F(TeamManagerRegistry, TeamMembersAreIndexedInRegistrationOrder)
{
	TestTeamMember a, b, c;
	world->ReferenceTeamMember(&a, 11);
	world->ReferenceTeamMember(&b, 22);
	world->ReferenceTeamMember(&c, 33);

	ASSERT_EQ(world->GetTeamMemberCount(), 3u);
	EXPECT_EQ(world->GetTeamMemberByIndex(0), &a);
	EXPECT_EQ(world->GetTeamMemberByIndex(1), &b);
	EXPECT_EQ(world->GetTeamMemberByIndex(2), &c);
	EXPECT_EQ(world->GetTeamMemberIndex(&b), 1u);
	EXPECT_EQ(world->GetTeamMemberIDByIndex(2), static_cast<NetworkID>(33));
}

TEST_F(TeamManagerRegistry, DereferencingATeamMemberShiftsTheRestDown)
{
	TestTeamMember a, b, c;
	world->ReferenceTeamMember(&a, 11);
	world->ReferenceTeamMember(&b, 22);
	world->ReferenceTeamMember(&c, 33);

	world->DereferenceTeamMember(&a);

	ASSERT_EQ(world->GetTeamMemberCount(), 2u);
	EXPECT_EQ(world->GetTeamMemberByIndex(0), &b) << "the remainder must shift down, not be swapped";
	EXPECT_EQ(world->GetTeamMemberByIndex(1), &c);
}

TEST_F(TeamManagerRegistry, TeamMembersAreFoundByNetworkId)
{
	TestTeamMember a, b;
	world->ReferenceTeamMember(&a, 11);
	world->ReferenceTeamMember(&b, 22);

	EXPECT_EQ(world->GetTeamMemberByNetworkID(11), &a);
	EXPECT_EQ(world->GetTeamMemberByNetworkID(22), &b);
	EXPECT_EQ(world->GetTeamMemberByNetworkID(999), (TM_TeamMember *) 0);

	world->DereferenceTeamMember(&a);
	EXPECT_EQ(world->GetTeamMemberByNetworkID(11), (TM_TeamMember *) 0);
}

TEST_F(TeamManagerRegistry, ParticipantsRoundTripAndDeduplicate)
{
	const RakNetGUID g1(1001), g2(1002);
	world->AddParticipant(g1);
	world->AddParticipant(g2);

	DataStructures::List<RakNetGUID> list;
	world->GetParticipantList(list);
	ASSERT_EQ(list.Size(), 2u);

	std::set<uint64_t> got;
	for (unsigned i = 0; i < list.Size(); ++i)
		got.insert(list[i].g);
	EXPECT_EQ(got.count(1001), 1u);
	EXPECT_EQ(got.count(1002), 1u);

	world->RemoveParticipant(g1);
	DataStructures::List<RakNetGUID> after;
	world->GetParticipantList(after);
	ASSERT_EQ(after.Size(), 1u);
	EXPECT_EQ(after[0].g, static_cast<uint64_t>(1002));
}

// RemoveWorld uses RemoveAtIndexFast while DereferenceTeam uses RemoveAtIndex. Both indices are
// published, so the two idioms have to stay distinct through the migration.
TEST_F(TeamManagerRegistry, RemoveWorldSwapsTheLastWorldIntoTheHole)
{
	manager.AddWorld(1);
	manager.AddWorld(2);
	manager.AddWorld(3);
	ASSERT_EQ(manager.GetWorldCount(), 4u);

	TM_World *w1 = manager.GetWorldWithId(1);
	TM_World *w2 = manager.GetWorldWithId(2);
	TM_World *w3 = manager.GetWorldWithId(3);
	ASSERT_NE(w1, (TM_World *) 0);
	ASSERT_EQ(manager.GetWorldAtIndex(1), w1);
	ASSERT_EQ(manager.GetWorldAtIndex(2), w2);
	ASSERT_EQ(manager.GetWorldAtIndex(3), w3);

	// Remove index 1 with TWO worlds after it. Removing the second-to-last would be degenerate:
	// swapping the last into the hole and shifting the remainder down give the same answer there,
	// so such a case cannot tell the two idioms apart.
	manager.RemoveWorld(1);

	ASSERT_EQ(manager.GetWorldCount(), 3u);
	EXPECT_EQ(manager.GetWorldAtIndex(1), w3)
		<< "the last world must be swapped into the hole, not shifted up";
	EXPECT_EQ(manager.GetWorldAtIndex(2), w2);
	EXPECT_EQ(manager.GetWorldWithId(1), (TM_World *) 0);
	EXPECT_EQ(manager.GetWorldWithId(3), w3);
}

TEST_F(TeamManagerRegistry, WorldsAreFoundById)
{
	manager.AddWorld(7);
	ASSERT_EQ(manager.GetWorldCount(), 2u);
	EXPECT_NE(manager.GetWorldWithId(7), (TM_World *) 0);
	EXPECT_EQ(manager.GetWorldWithId(9), (TM_World *) 0);
}
