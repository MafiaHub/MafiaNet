/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// TeamManager's list queries fill std::vector out-parameters. One host peer on the simulated network:
// requests a host makes of itself loop back through its own update cycle. Also TeamBalancer's
// balancing helpers, which take and fill std::vector.

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/team_balancer.h"
#include "mafianet/team_manager.h"

#include <vector>

using namespace MafiaNet;

namespace
{
class TeamManagerApi : public ::testing::Test
{
protected:
	void SetUp() override
	{
		host = net.AddPeer(4, 4);
		ASSERT_NE(host, nullptr);
		host->AttachPlugin(&teamManager);
		world = teamManager.AddWorld(0);
		ASSERT_NE(world, nullptr);
		world->SetHost(host->GetMyGUID());
		world->ReferenceTeam(&red, 1, false);
		world->ReferenceTeam(&blue, 2, false);
		world->ReferenceTeamMember(&alice, 10);
		world->ReferenceTeamMember(&bob, 11);
	}

	void TearDown() override
	{
		if (host != nullptr)
			host->DetachPlugin(&teamManager);
	}

	// Lets loopback requests to the host (ourselves) arrive and be processed.
	void Settle() { net.Step(50); }

	SimulatedNetwork net;
	RakPeer *host = nullptr;
	TeamManager teamManager;
	TM_World *world = nullptr;
	// Declared after the manager: destroyed first, while their world still exists.
	TM_Team red;
	TM_Team blue;
	TM_TeamMember alice;
	TM_TeamMember bob;
};
} // namespace

TEST_F(TeamManagerApi, GetTeamMembersFillsVector)
{
	ASSERT_TRUE(alice.RequestTeam(TeamSelection::SpecificTeam(&red)));
	ASSERT_TRUE(bob.RequestTeam(TeamSelection::SpecificTeam(&red)));
	Settle();

	std::vector<TM_TeamMember *> members = {nullptr};
	red.GetTeamMembers(members);
	EXPECT_EQ(members, (std::vector<TM_TeamMember *>{&alice, &bob}));

	blue.GetTeamMembers(members);
	EXPECT_TRUE(members.empty());
}

TEST_F(TeamManagerApi, CurrentAndLastTeamsFillVectors)
{
	ASSERT_TRUE(alice.RequestTeam(TeamSelection::SpecificTeam(&red)));
	Settle();

	std::vector<TM_Team *> teams;
	alice.GetCurrentTeams(teams);
	EXPECT_EQ(teams, (std::vector<TM_Team *>{&red}));

	// Leaving for no team applies locally at once, recording the teams left.
	ASSERT_TRUE(alice.RequestTeam(TeamSelection::NoTeam(0)));
	alice.GetCurrentTeams(teams);
	EXPECT_TRUE(teams.empty());
	teams = {nullptr, nullptr};
	alice.GetLastTeams(teams);
	EXPECT_EQ(teams, (std::vector<TM_Team *>{&red}));
}

// A request for a full team stays pending; once a slot opens, the oldest request is served first.
TEST_F(TeamManagerApi, PendingRequestsFillVectorAndAreServedOldestFirst)
{
	ASSERT_TRUE(blue.SetMemberLimit(0, 0));
	Settle();
	ASSERT_TRUE(bob.RequestTeam(TeamSelection::SpecificTeam(&blue)));
	Settle();
	ASSERT_TRUE(alice.RequestTeam(TeamSelection::SpecificTeam(&blue)));
	Settle();

	std::vector<TM_Team *> requested = {nullptr, nullptr};
	alice.GetRequestedSpecificTeams(requested);
	EXPECT_EQ(requested, (std::vector<TM_Team *>{&blue}));
	EXPECT_FALSE(alice.IsOnTeam(&blue));
	EXPECT_FALSE(bob.IsOnTeam(&blue));

	ASSERT_TRUE(blue.SetMemberLimit(1, 0));
	Settle();
	EXPECT_TRUE(bob.IsOnTeam(&blue)) << "the older request must be served first";
	EXPECT_FALSE(alice.IsOnTeam(&blue));
	alice.GetRequestedSpecificTeams(requested);
	EXPECT_EQ(requested, (std::vector<TM_Team *>{&blue}));
	bob.GetRequestedSpecificTeams(requested);
	EXPECT_TRUE(requested.empty());
}

TEST_F(TeamManagerApi, ParticipantListFillsVector)
{
	world->AddParticipant(RakNetGUID(500));
	world->AddParticipant(RakNetGUID(400));
	std::vector<RakNetGUID> participants = {RakNetGUID(1)};
	world->GetParticipantList(participants);
	EXPECT_EQ(participants, (std::vector<RakNetGUID>{RakNetGUID(500), RakNetGUID(400)}));

	world->RemoveParticipant(RakNetGUID(500));
	world->GetParticipantList(participants);
	EXPECT_EQ(participants, (std::vector<RakNetGUID>{RakNetGUID(400)}));
}

// The networkID lookup tables must not keep pointers past Clear().
TEST_F(TeamManagerApi, ClearEmptiesTheNetworkIdLookups)
{
	ASSERT_EQ(world->GetTeamByNetworkID(1), &red);
	ASSERT_EQ(world->GetTeamMemberByNetworkID(10), &alice);
	world->Clear();
	EXPECT_EQ(world->GetTeamByNetworkID(1), nullptr);
	EXPECT_EQ(world->GetTeamMemberByNetworkID(10), nullptr);
}

namespace
{
// TeamBalancer's balancing helpers are protected; this exposes them.
class ExposedTeamBalancer : public TeamBalancer
{
public:
	using TeamBalancer::GetMemberIndexToSwitchTeams;
	using TeamBalancer::GetOverpopulatedTeams;

	unsigned int Add(NetworkID memberId, TeamId currentTeam, TeamId requestedTeam)
	{
		TeamMember member;
		member.memberGuid = RakNetGUID(memberId);
		member.memberId = memberId;
		member.currentTeam = currentTeam;
		member.requestedTeam = requestedTeam;
		return AddTeamMember(member);
	}
};
} // namespace

TEST(TeamBalancerApi, BalancingHelpersUseVectors)
{
	ExposedTeamBalancer balancer;
	balancer.SetTeamSizeLimit(0, 4);
	balancer.SetTeamSizeLimit(1, 4);
	EXPECT_EQ(balancer.Add(1, 0, 1), 0u); // wants to move to team 1
	EXPECT_EQ(balancer.Add(2, 0, UNASSIGNED_TEAM_ID), 1u);
	EXPECT_EQ(balancer.Add(3, 0, UNASSIGNED_TEAM_ID), 2u);
	EXPECT_EQ(balancer.Add(4, 1, UNASSIGNED_TEAM_ID), 3u);

	std::vector<TeamId> overpopulated = {7, 7};
	balancer.GetOverpopulatedTeams(overpopulated, 2);
	EXPECT_EQ(overpopulated, std::vector<TeamId>{0});

	// The member who asked for the target team is preferred over the others.
	EXPECT_EQ(balancer.GetMemberIndexToSwitchTeams(std::vector<TeamId>{0}, 1), 0u);
	// Without such a request, any member of a source team; none at all gives (unsigned int) -1.
	EXPECT_EQ(balancer.GetMemberIndexToSwitchTeams(std::vector<TeamId>{1}, 0), 3u);
	balancer.SetTeamSizeLimit(2, 4);
	EXPECT_EQ(balancer.GetMemberIndexToSwitchTeams(std::vector<TeamId>{2}, 0), (unsigned int) -1);
}

// SetTeamSizeLimit() grows the per-team tables to the team it names, and a member added to the last
// team stays in bounds.
TEST(TeamBalancerApi, TeamTablesGrowToTheHighestTeam)
{
	ExposedTeamBalancer balancer;
	balancer.SetTeamSizeLimit(3, 2);
	EXPECT_EQ(balancer.Add(1, 3, UNASSIGNED_TEAM_ID), 0u);
	EXPECT_EQ(balancer.Add(2, 3, UNASSIGNED_TEAM_ID), 1u);
	std::vector<TeamId> overpopulated;
	balancer.GetOverpopulatedTeams(overpopulated, 2);
	EXPECT_EQ(overpopulated, std::vector<TeamId>{3});
}
