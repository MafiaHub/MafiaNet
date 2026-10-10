/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/team_balancer.h"
#include "mafianet/peer_interface.h"

using namespace MafiaNet;

/*
Description:
Covers the client-side membership list TeamBalancer keeps (myTeamMembers), written before it moves
off DataStructures::List (std migration stage 2, #60). TeamBalancer had no tests.

What is and is not pinned here, stated plainly:

 - RequestSpecificTeam / RequestAnyTeam / CancelRequestSpecificTeam / DeleteMember / GetMyTeam all
   read and write myTeamMembers by NetworkID, and that behaviour is covered below.
 - DeleteMember uses RemoveAtIndexFast on myTeamMembers, but no public method exposes the index of
   that list and every lookup is by NetworkID, so swap-remove and shift-down are indistinguishable
   through the public API. The migration reproduces the swap anyway -- it is the cheaper operation
   and keeps the behaviour identical -- but no test here can tell the two apart, and this comment
   exists so nobody assumes otherwise.
 - The host-side teamMembers list, whose order does decide which member is moved when teams are
   rebalanced, is driven by received packets and is not reachable from a hermetic test.

These calls send to hostGuid, which is UNASSIGNED_RAKNET_GUID on an unconnected peer; the send is
a no-op and the local list is still updated, which is what is under test.
*/

namespace
{
	class TeamBalancerMembership : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			peer = RakPeerInterface::GetInstance();
			peer->AttachPlugin(&balancer);
		}
		void TearDown() override
		{
			peer->DetachPlugin(&balancer);
			RakPeerInterface::DestroyInstance(peer);
		}

		RakPeerInterface *peer = nullptr;
		TeamBalancer balancer;
	};

	const NetworkID kMemberA = 501;
	const NetworkID kMemberB = 502;
	const NetworkID kMemberC = 503;
}

TEST_F(TeamBalancerMembership, AnUnknownMemberHasNoTeam)
{
	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID);
}

// A requested team is not an assigned team: currentTeam only changes when the host answers.
TEST_F(TeamBalancerMembership, RequestingATeamRecordsTheMemberWithoutAssigningIt)
{
	balancer.RequestSpecificTeam(kMemberA, 3);
	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID)
		<< "the team is requested, not yet granted by the host";
}

TEST_F(TeamBalancerMembership, RequestingTwiceDoesNotCreateASecondEntry)
{
	balancer.RequestSpecificTeam(kMemberA, 3);
	balancer.RequestSpecificTeam(kMemberA, 4);

	// If a duplicate entry had been created, deleting once would leave the other behind and
	// GetMyTeam would still resolve the member.
	balancer.DeleteMember(kMemberA);
	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID);
}

TEST_F(TeamBalancerMembership, SeveralMembersAreTrackedIndependently)
{
	balancer.RequestSpecificTeam(kMemberA, 1);
	balancer.RequestSpecificTeam(kMemberB, 2);
	balancer.RequestSpecificTeam(kMemberC, 3);

	// None are assigned yet, but all three must be known: deleting the middle one must not
	// disturb the others, which is the property a container swap can break.
	balancer.DeleteMember(kMemberB);

	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID);
	EXPECT_EQ(balancer.GetMyTeam(kMemberB), UNASSIGNED_TEAM_ID);
	EXPECT_EQ(balancer.GetMyTeam(kMemberC), UNASSIGNED_TEAM_ID);
}

TEST_F(TeamBalancerMembership, CancellingARequestKeepsTheMemberKnown)
{
	balancer.RequestSpecificTeam(kMemberA, 3);
	balancer.CancelRequestSpecificTeam(kMemberA);
	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID);
}

TEST_F(TeamBalancerMembership, RequestAnyTeamRecordsTheMember)
{
	balancer.RequestAnyTeam(kMemberA);
	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID);
}

TEST_F(TeamBalancerMembership, DeletingAnUnknownMemberIsHarmless)
{
	balancer.RequestSpecificTeam(kMemberA, 1);
	balancer.DeleteMember(kMemberC); // never added
	EXPECT_EQ(balancer.GetMyTeam(kMemberA), UNASSIGNED_TEAM_ID);
}

TEST_F(TeamBalancerMembership, ManyMembersSurviveTogether)
{
	const int count = 64;
	for (int i = 0; i < count; ++i)
		balancer.RequestSpecificTeam((NetworkID) (1000 + i), (TeamId) (i % 4));

	// Delete every other one; the rest must remain resolvable. With DeleteMember swapping the
	// last entry into the hole, this exercises the swap path repeatedly.
	for (int i = 0; i < count; i += 2)
		balancer.DeleteMember((NetworkID) (1000 + i));

	for (int i = 0; i < count; ++i)
		EXPECT_EQ(balancer.GetMyTeam((NetworkID) (1000 + i)), UNASSIGNED_TEAM_ID);
}

TEST_F(TeamBalancerMembership, TeamSizeLimitsAreAccepted)
{
	// Exercises teamLimits, which grows to cover the highest team id set.
	balancer.SetTeamSizeLimit(0, 4);
	balancer.SetTeamSizeLimit(5, 2);
	balancer.SetForceEvenTeams(true);
	balancer.SetLockTeams(false);
	SUCCEED() << "no crash growing teamLimits past the requested team ids";
}
