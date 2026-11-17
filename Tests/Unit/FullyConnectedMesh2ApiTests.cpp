/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// FullyConnectedMesh2 and ConnectionGraph2 list queries, on a three-peer mesh on the simulated network.

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/connection_graph2.h"
#include "mafianet/fully_connected_mesh2.h"
#include "mafianet/message_identifiers.h"

#include <algorithm>
#include <set>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

// ConnectionGraph2 owns its remote systems through std::unique_ptr. Its copy operations must be
// explicitly deleted: as an exported class, MSVC instantiates a declared copy in the DLL build.
static_assert(!std::is_copy_constructible<ConnectionGraph2>::value, "ConnectionGraph2 must not be copy-constructible");
static_assert(!std::is_copy_assignable<ConnectionGraph2>::value, "ConnectionGraph2 must not be copy-assignable");

namespace
{
constexpr int kPeers = 3;

class FCM2Api : public ::testing::Test
{
protected:
	void SetUp() override
	{
		for (int i = 0; i < kPeers; i++)
		{
			peers[i] = net.AddPeer(kPeers, kPeers);
			ASSERT_NE(peers[i], nullptr);
			peers[i]->AttachPlugin(&fcm2[i]);
			peers[i]->AttachPlugin(&graph[i]);
		}
		// Everyone joins through peer 0; ConnectionGraph2 tells them about each other and FCM2
		// connects them, completing the mesh.
		for (int i = 1; i < kPeers; i++)
			ASSERT_TRUE(net.Connect(peers[i], peers[0])) << "peer " << i << " -> peer 0";
		ASSERT_TRUE(net.RunUntil([this]
		                         { return MeshComplete() && AllAgreeOnHost(); }, 10000))
		    << "mesh never completed";
	}

	bool MeshComplete()
	{
		for (int i = 0; i < kPeers; i++)
			if (fcm2[i].GetParticipantCount() != (unsigned int) (kPeers - 1))
				return false;
		return true;
	}

	void TearDown() override
	{
		for (int i = 0; i < kPeers; i++)
		{
			if (peers[i] == nullptr)
				continue;
			peers[i]->DetachPlugin(&graph[i]);
			peers[i]->DetachPlugin(&fcm2[i]);
		}
	}

	bool AllAgreeOnHost()
	{
		const RakNetGUID host = fcm2[0].GetConnectedHost();
		if (host == UNASSIGNED_RAKNET_GUID)
			return false;
		for (int i = 1; i < kPeers; i++)
			if (fcm2[i].GetConnectedHost() != host)
				return false;
		return true;
	}

	std::vector<RakNetGUID> OthersOf(int self) const
	{
		std::vector<RakNetGUID> others;
		for (int i = 0; i < kPeers; i++)
			if (i != self)
				others.push_back(peers[i]->GetMyGUID());
		std::sort(others.begin(), others.end());
		return others;
	}

	FullyConnectedMesh2 fcm2[kPeers];
	ConnectionGraph2 graph[kPeers];
	SimulatedNetwork net;
	RakPeer *peers[kPeers] = {};
};
} // namespace

TEST_F(FCM2Api, GetParticipantListMatchesConnectedPeers)
{
	for (int i = 0; i < kPeers; i++)
	{
		std::vector<RakNetGUID> participants = {UNASSIGNED_RAKNET_GUID};
		fcm2[i].GetParticipantList(participants);
		std::sort(participants.begin(), participants.end());
		EXPECT_EQ(participants, OthersOf(i)) << "peer " << i;
	}
}

// Every peer computes the same host order, the host first.
TEST_F(FCM2Api, GetHostOrderIsTheSameEverywhereWithTheHostFirst)
{
	std::vector<RakNetGUID> reference;
	fcm2[0].GetHostOrder(reference);
	ASSERT_EQ(reference.size(), (size_t) kPeers);
	EXPECT_EQ(reference[0], fcm2[0].GetConnectedHost());
	std::vector<RakNetGUID> everyone = reference;
	std::sort(everyone.begin(), everyone.end());
	std::vector<RakNetGUID> expected = OthersOf(0);
	expected.push_back(peers[0]->GetMyGUID());
	std::sort(expected.begin(), expected.end());
	EXPECT_EQ(everyone, expected);

	for (int i = 1; i < kPeers; i++)
	{
		std::vector<RakNetGUID> order = {UNASSIGNED_RAKNET_GUID};
		fcm2[i].GetHostOrder(order);
		EXPECT_EQ(order, reference) << "peer " << i;
	}
}

// When a participant drops, the others list and host order shrink with it.
TEST_F(FCM2Api, ParticipantLeavingShrinksTheLists)
{
	peers[2]->CloseConnection(peers[0]->GetMyGUID(), true);
	peers[2]->CloseConnection(peers[1]->GetMyGUID(), true);
	std::vector<RakNetGUID> participants;
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		                         fcm2[0].GetParticipantList(participants);
		                         return participants.size() == 1; },
	                         5000));
	EXPECT_EQ(participants, std::vector<RakNetGUID>{peers[1]->GetMyGUID()});

	std::vector<RakNetGUID> order;
	fcm2[0].GetHostOrder(order);
	EXPECT_EQ(order.size(), 2u);
	EXPECT_EQ(std::count(order.begin(), order.end(), peers[2]->GetMyGUID()), 0);
}

// The verified-join readers fill std::vectors, replacing their contents.
TEST_F(FCM2Api, VerifiedJoinReadersFillVectors)
{
	// No join in progress for this host: the processing lists come back empty.
	std::vector<SystemAddress> addresses = {SystemAddress()};
	std::vector<RakNetGUID> guids = {UNASSIGNED_RAKNET_GUID};
	std::vector<BitStream *> userData;
	fcm2[0].GetVerifiedJoinRequiredProcessingList(peers[1]->GetMyGUID(), addresses, guids, userData);
	EXPECT_TRUE(addresses.empty());
	EXPECT_TRUE(guids.empty());
	EXPECT_TRUE(userData.empty());

	// An ID_FCM2_VERIFIED_JOIN_ACCEPTED naming this system: the systems it adds, then the sender.
	BitStream accepted;
	accepted.Write((MessageID) ID_FCM2_VERIFIED_JOIN_ACCEPTED);
	accepted.Write(peers[0]->GetMyGUID());
	accepted.Write((unsigned short) 0); // nobody to disconnect from
	accepted.Write((unsigned short) 2);
	accepted.Write(RakNetGUID(71));
	accepted.Write(RakNetGUID(72));
	Packet packet;
	packet.data = accepted.GetData();
	packet.length = accepted.GetNumberOfBytesUsed();
	packet.guid = RakNetGUID(70);

	bool thisSystemAccepted = false;
	std::vector<RakNetGUID> systemsAccepted = {UNASSIGNED_RAKNET_GUID};
	fcm2[0].GetVerifiedJoinAcceptedAdditionalData(&packet, &thisSystemAccepted, systemsAccepted, nullptr);
	EXPECT_TRUE(thisSystemAccepted);
	EXPECT_EQ(systemsAccepted, (std::vector<RakNetGUID>{RakNetGUID(71), RakNetGUID(72), RakNetGUID(70)}));

	// The same message seen by another system names just the one accepted.
	fcm2[1].GetVerifiedJoinAcceptedAdditionalData(&packet, &thisSystemAccepted, systemsAccepted, nullptr);
	EXPECT_FALSE(thisSystemAccepted);
	EXPECT_EQ(systemsAccepted, std::vector<RakNetGUID>{peers[0]->GetMyGUID()});
}

// ConnectionGraph2 hands out its participants as a std::set, in RakNetGUID order.
TEST_F(FCM2Api, ConnectionGraphParticipantListIsASet)
{
	for (int i = 0; i < kPeers; i++)
	{
		std::set<RakNetGUID> participants = {RakNetGUID(1)};
		graph[i].GetParticipantList(participants);
		const std::vector<RakNetGUID> others = OthersOf(i);
		EXPECT_EQ(participants, std::set<RakNetGUID>(others.begin(), others.end())) << "peer " << i;
	}

	// Each graph learns, from its neighbours, who they are connected to.
	ASSERT_TRUE(net.RunUntil([this]
	                         { return graph[0].ConnectionExists(peers[1]->GetMyGUID(), peers[2]->GetMyGUID()); }, 5000));
	SystemAddress addresses[kPeers];
	RakNetGUID guids[kPeers];
	unsigned int length = kPeers;
	ASSERT_TRUE(graph[0].GetConnectionListForRemoteSystem(peers[1]->GetMyGUID(), addresses, guids, &length));
	// What peer 1 told us it is connected to (peer 2 at least), in RakNetGUID order.
	ASSERT_GE(length, 1u);
	std::vector<RakNetGUID> listed(guids, guids + length);
	EXPECT_TRUE(std::is_sorted(listed.begin(), listed.end()));
	EXPECT_EQ(std::count(listed.begin(), listed.end(), peers[2]->GetMyGUID()), 1);
}
