/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <stdint.h>

#include "mafianet/session_admission.h"
#include "mafianet/peer_handle.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/defines.h"

using namespace MafiaNet;

namespace
{
class SessionAdmissionPeer : public ::testing::Test
{
public:
	void SetUp() override
	{
		peer = RakPeerInterface::GetInstance();
		ASSERT_NE(peer, nullptr);
	}

	void TearDown() override
	{
		if (peer)
		{
			peer->Shutdown(100);
			RakPeerInterface::DestroyInstance(peer);
			peer = 0;
		}
	}

	RakPeerInterface *peer = 0;
};
} // namespace

TEST(ServerCapacity, ValidatesPhysicalSlotsAndReservedSubsetBeforeStartup)
{
	ServerCapacity capacity{64, 32, 4, 2};
	EXPECT_TRUE(capacity.valid());
	EXPECT_TRUE(capacity.allows_pending(31, 1));
	EXPECT_FALSE(capacity.allows_pending(32, 1));
	EXPECT_FALSE(capacity.allows_pending(5, 2));
	for (const auto invalid : {ServerCapacity{0}, ServerCapacity{1, 0}, ServerCapacity{1, 1, 2}, ServerCapacity{1, 1, 0, 2}, ServerCapacity{65535, 1}, ServerCapacity{1, UINT32_MAX}})
	{
		EXPECT_FALSE(invalid.valid());
		auto result = Peer::server().capacity(invalid).start();
		ASSERT_FALSE(result);
		EXPECT_EQ(result.error().startup, INVALID_MAX_CONNECTIONS);
	}
}

TEST_F(SessionAdmissionPeer, PendingSessionLimitsCanBeSetBeforeStartup)
{
	peer->SetMaximumPendingSessions(16, 2);
	peer->SetMaximumPendingSessions(0, 5);
	peer->SetSessionTimeout(45000);
	peer->SetSessionTimeout(0);
	SUCCEED();
}

TEST_F(SessionAdmissionPeer, SessionStatusToAnUnknownSystemIsANoOp)
{
	peer->SendSessionStatus(UNASSIGNED_SYSTEM_ADDRESS, "queued", 6);
	peer->SendSessionStatus(UNASSIGNED_RAKNET_GUID, 0, 0);
	SUCCEED();
}

TEST_F(SessionAdmissionPeer, SessionStatusOnAStartedPeerWithNoConnectionsIsANoOp)
{
	SocketDescriptor sd(0, "127.0.0.1");
	ASSERT_EQ(peer->Startup(4, &sd, 1), RAKNET_STARTED);
	peer->SetMaximumIncomingConnections(2);
	peer->SetMaximumPendingSessions(2, 1);
	peer->SendSessionStatus(UNASSIGNED_SYSTEM_ADDRESS, "queued", 6);
	EXPECT_EQ(peer->NumberOfConnections(), 0u);
}

// ---- message ids --------------------------------------------------------------------------------------------

TEST(SessionAdmissionMessageIds, AreBelowUserPacketEnum)
{
	EXPECT_LT((int) ID_SESSION_CONFIG_STATUS, (int) ID_USER_PACKET_ENUM);
	EXPECT_LT((int) ID_SESSION_CONFIG_ABANDONED, (int) ID_USER_PACKET_ENUM);
}

TEST(SessionAdmissionMessageIds, FinishTheBuiltInMessageRange)
{
	EXPECT_EQ((int) ID_SESSION_CONFIG_STATUS, (int) ID_SESSION_CONFIG_REJECTED + 1);
	EXPECT_EQ((int) ID_SESSION_CONFIG_ABANDONED, (int) ID_SESSION_CONFIG_REJECTED + 2);
	EXPECT_EQ((int) ID_USER_PACKET_ENUM, (int) ID_SESSION_CONFIG_REJECTED + 3);
}

TEST(SessionAdmissionMessageIds, AreDistinctFromTheOtherHandshakeIds)
{
	const int ids[] = {ID_SESSION_CONFIG_REQUEST, ID_SESSION_CONFIG, ID_SESSION_CONFIG_REJECTED, ID_SESSION_CONFIG_STATUS, ID_SESSION_CONFIG_ABANDONED};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
		for (size_t j = i + 1; j < sizeof(ids) / sizeof(ids[0]); ++j)
			EXPECT_NE(ids[i], ids[j]);
}
