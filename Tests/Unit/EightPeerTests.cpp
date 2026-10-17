/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Eight peers on the SimulatedNetwork form a full mesh, then every peer
 *  broadcasts numbered ReliableOrdered messages to all the others.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
	const int kPeerNum = 8;
	const int kNumPackets = 100;
	const MessageID ID_MESH_BROADCAST = ID_USER_PACKET_ENUM + 1;

	int CountOf(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
	{
		int count = 0;
		for (const auto &message : net.Inbox(peer))
			if (message.id == id)
				++count;
		return count;
	}

	// Messages that must never show up in this scenario.
	const MessageID kFailureIds[] = {
		ID_REMOTE_DISCONNECTION_NOTIFICATION,
		ID_REMOTE_CONNECTION_LOST,
		ID_CONNECTION_ATTEMPT_FAILED,
		ID_NO_FREE_INCOMING_CONNECTIONS,
		ID_ALREADY_CONNECTED,
		ID_DISCONNECTION_NOTIFICATION,
		ID_CONNECTION_LOST,
	};

	class EightPeerSim : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			for (int i = 0; i < kPeerNum; ++i)
				peers[i] = net.AddPeer(kPeerNum * 2, kPeerNum);
		}

		int ConnectionsSeen(int i)
		{
			return CountOf(net, peers[i], ID_CONNECTION_REQUEST_ACCEPTED) +
				CountOf(net, peers[i], ID_NEW_INCOMING_CONNECTION);
		}

		void ExpectNoFailures()
		{
			for (int i = 0; i < kPeerNum; ++i)
				for (MessageID id : kFailureIds)
					EXPECT_FALSE(net.Received(peers[i], id)) << "peer " << i << " received message id " << (int) id;
		}

		void ConnectMesh()
		{
			for (int i = 0; i < kPeerNum; ++i)
				for (int j = i + 1; j < kPeerNum; ++j)
					ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(peers[i], peers[j])) << i << " -> " << j;

			const bool allConnected = net.RunUntil([&] {
				for (int i = 0; i < kPeerNum; ++i)
					if (ConnectionsSeen(i) < kPeerNum - 1)
						return false;
				return true;
			}, 20000);
			ASSERT_TRUE(allConnected) << "peers failed to connect to all peers within 20s";
		}

		SimulatedNetwork net;
		RakPeer *peers[kPeerNum] = {};
	};
}

TEST_F(EightPeerSim, FullMeshConnectsWithoutFailures)
{
	ASSERT_NO_FATAL_FAILURE(ConnectMesh());
	for (int i = 0; i < kPeerNum; ++i)
	{
		EXPECT_EQ(kPeerNum - 1, ConnectionsSeen(i)) << "peer " << i;
		DataStructures::List<SystemAddress> systemList;
		DataStructures::List<RakNetGUID> guidList;
		peers[i]->GetSystemList(systemList, guidList);
		EXPECT_EQ((unsigned) (kPeerNum - 1), guidList.Size()) << "peer " << i;
	}
	ExpectNoFailures();
}

TEST_F(EightPeerSim, ReliableOrderedBroadcastArrivesCompleteAndInOrder)
{
	ASSERT_NO_FATAL_FAILURE(ConnectMesh());

	for (int k = 0; k < kNumPackets; ++k)
	{
		for (int i = 0; i < kPeerNum; ++i)
		{
			BitStream bs;
			bs.Write(ID_MESH_BROADCAST);
			bs.Write(k);
			bs.Write(i);
			ASSERT_NE(0u, peers[i]->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true));
		}
		net.Step(10);
	}

	ASSERT_TRUE(net.RunUntil([&] {
		for (int i = 0; i < kPeerNum; ++i)
			if (CountOf(net, peers[i], ID_MESH_BROADCAST) < kNumPackets * (kPeerNum - 1))
				return false;
		return true;
	}, 5000));

	for (int i = 0; i < kPeerNum; ++i)
	{
		int nextFrom[kPeerNum] = {};
		for (const auto &message : net.Inbox(peers[i]))
		{
			if (message.id != ID_MESH_BROADCAST)
				continue;
			BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
			bs.IgnoreBytes(1);
			int sequence = -1, sender = -1;
			ASSERT_TRUE(bs.Read(sequence));
			ASSERT_TRUE(bs.Read(sender));
			ASSERT_TRUE(sender >= 0 && sender < kPeerNum && sender != i) << "bad sender " << sender;
			ASSERT_EQ(nextFrom[sender], sequence) << "peer " << i << ": out of order from " << sender;
			++nextFrom[sender];
		}
		for (int j = 0; j < kPeerNum; ++j)
			if (j != i)
				EXPECT_EQ(kNumPackets, nextFrom[j]) << "peer " << i << " received " << nextFrom[j] << " from " << j;
	}
	ExpectNoFailures();
}
