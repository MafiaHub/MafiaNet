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

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
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

namespace
{
// The host side of TeamBalancer fed crafted requests through OnReceive, as a remote member's
// messages would arrive. Two teams, 0 and 1: a team id of 2 (== the team count) is out of range.
class TeamBalancerHostReceiving : public ::testing::Test
{
protected:
	// TeamBalancerOperations in team_balancer.cpp, the byte after ID_TEAM_BALANCER_INTERNAL.
	static constexpr unsigned char kStatusUpdateToNewHost = 0;
	static constexpr unsigned char kRequestSpecificTeam = 3;

	class HostBalancer : public ExposedTeamBalancer
	{
	public:
		using TeamBalancer::OnReceive;
		size_t MemberCount() const { return teamMembers.size(); }
	};

	void SetUp() override
	{
		host = net.AddPeer(4, 4);
		ASSERT_NE(host, nullptr);
		host->AttachPlugin(&balancer);
		balancer.SetTeamSizeLimit(0, 4);
		balancer.SetTeamSizeLimit(1, 4);
		// The host announces itself the way FullyConnectedMesh2 does.
		BitStream newHost;
		newHost.Write((MessageID) ID_FCM2_NEW_HOST);
		Deliver(newHost, host->GetMyGUID());
	}

	void TearDown() override
	{
		if (host != nullptr)
			host->DetachPlugin(&balancer);
	}

	void Deliver(BitStream &bs, RakNetGUID from)
	{
		Packet packet;
		packet.data = bs.GetData();
		packet.length = bs.GetNumberOfBytesUsed();
		packet.guid = from;
		packet.systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
		balancer.OnReceive(&packet);
	}

	void RequestSpecificTeam(NetworkID memberId, TeamId team)
	{
		BitStream bs;
		bs.Write((MessageID) ID_TEAM_BALANCER_INTERNAL);
		bs.Write((MessageID) kRequestSpecificTeam);
		bs.Write(memberId);
		bs.Write(team);
		Deliver(bs, remote);
	}

	void StatusUpdateToNewHost(NetworkID memberId, TeamId currentTeam, TeamId requestedTeam)
	{
		BitStream bs;
		bs.Write((MessageID) ID_TEAM_BALANCER_INTERNAL);
		bs.Write((MessageID) kStatusUpdateToNewHost);
		bs.Write((uint8_t) 1);
		bs.Write(memberId);
		bs.Write(currentTeam);
		bs.Write(requestedTeam);
		Deliver(bs, remote);
	}

	HostBalancer balancer;
	SimulatedNetwork net;
	RakPeer *host = nullptr;
	const RakNetGUID remote = RakNetGUID(777);
};
} // namespace

// A request naming the team one past the last (team id == team count) is dropped; the last valid
// team is accepted.
TEST_F(TeamBalancerHostReceiving, SpecificTeamEqualToTeamCountIsRejected)
{
	RequestSpecificTeam(1, 2);
	EXPECT_EQ(balancer.MemberCount(), 0u) << "team 2 of 2 teams must be rejected";

	RequestSpecificTeam(1, 1);
	EXPECT_EQ(balancer.MemberCount(), 1u);
}

TEST_F(TeamBalancerHostReceiving, StatusUpdateWithTeamEqualToTeamCountIsRejected)
{
	StatusUpdateToNewHost(1, 2, UNASSIGNED_TEAM_ID);
	EXPECT_EQ(balancer.MemberCount(), 0u) << "current team 2 of 2 teams must be rejected";

	StatusUpdateToNewHost(1, UNASSIGNED_TEAM_ID, 2);
	EXPECT_EQ(balancer.MemberCount(), 0u) << "requested team 2 of 2 teams must be rejected";

	StatusUpdateToNewHost(1, 1, UNASSIGNED_TEAM_ID);
	EXPECT_EQ(balancer.MemberCount(), 1u);
}

// Members added directly are refused for a team that has no size limit set.
TEST(TeamBalancerApi, AddTeamMemberRefusesUnknownTeams)
{
	ExposedTeamBalancer balancer;
	balancer.SetTeamSizeLimit(0, 4);
	balancer.SetTeamSizeLimit(1, 4);
	EXPECT_EQ(balancer.Add(1, 2, UNASSIGNED_TEAM_ID), (unsigned int) -1);
	EXPECT_EQ(balancer.Add(2, UNASSIGNED_TEAM_ID, UNASSIGNED_TEAM_ID), (unsigned int) -1);
	EXPECT_EQ(balancer.Add(3, 1, UNASSIGNED_TEAM_ID), 0u);
}
