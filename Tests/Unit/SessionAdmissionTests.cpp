/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <stdint.h>

#include "mafianet/SessionAdmission.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/defines.h"

using namespace MafiaNet;

namespace
{
	SessionAdmission::Limits Legacy(unsigned int maxIncoming)
	{
		SessionAdmission::Limits limits;
		limits.maxIncoming = maxIncoming;
		return limits;
	}

	SessionAdmission::Limits Pool(unsigned int maxIncoming, unsigned int maxPending, unsigned int perAddress)
	{
		SessionAdmission::Limits limits;
		limits.maxIncoming = maxIncoming;
		limits.maxPending = maxPending;
		limits.maxPendingPerAddress = perAddress;
		return limits;
	}

	SessionAdmission::Counts Counts(unsigned int connected, unsigned int exchanging, unsigned int pending, unsigned int pendingFromAddress)
	{
		SessionAdmission::Counts counts;
		counts.connected = connected;
		counts.exchanging = exchanging;
		counts.pending = pending;
		counts.pendingFromAddress = pendingFromAddress;
		return counts;
	}

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

// ---- without a pool: the historic rule, unchanged ------------------------------------------------------

TEST(SessionAdmission, WithoutAPoolTheLimitCountsConnectedAndHandshakingPeers)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Legacy(3), Counts(1, 1, 1, 0)));
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Legacy(3), Counts(2, 1, 1, 0)));
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Legacy(3), Counts(0, 3, 3, 0)));
}

// Peers still proving the transport never counted before the pool existed; the default must not start
// counting them either, or a server that never opts in behaves differently.
TEST(SessionAdmission, WithoutAPoolPeersBeforeTheHandshakeDoNotCount)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Legacy(2), Counts(1, 0, 40, 40)));
}

TEST(SessionAdmission, WithoutAPoolAZeroLimitAdmitsNobody)
{
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Legacy(0), Counts(0, 0, 0, 0)));
}

TEST(SessionAdmission, WithoutAPoolAcceptanceIsNeverRefused)
{
	// The slot was taken when the peer started connecting, so there is nothing left to check.
	EXPECT_TRUE(SessionAdmission::MayAccept(Legacy(1), Counts(1, 1, 1, 0)));
	EXPECT_TRUE(SessionAdmission::MayAccept(Legacy(0), Counts(5, 0, 0, 0)));
}

TEST(SessionAdmission, TheDefaultLimitsHaveNoPool)
{
	EXPECT_FALSE(SessionAdmission::UsesPendingPool(SessionAdmission::Limits()));
	EXPECT_TRUE(SessionAdmission::UsesPendingPool(Pool(1, 1, 0)));
}

// ---- with a pool -----------------------------------------------------------------------------------------

// The point of the pool: a full server still lets people start connecting, so it can hold and queue them.
TEST(SessionAdmission, WithAPoolAFullServerStillAdmitsHandshakes)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Pool(4, 8, 0), Counts(4, 0, 0, 0)));
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Pool(4, 8, 0), Counts(40, 0, 0, 0)));
}

TEST(SessionAdmission, WithAPoolHandshakesAreBoundedByThePool)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Pool(4, 3, 0), Counts(0, 2, 2, 0)));
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Pool(4, 3, 0), Counts(0, 2, 3, 0)));
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Pool(4, 3, 0), Counts(0, 0, 3, 0)));
}

// Every pre-connected state counts against the pool, not only the handshake itself: otherwise a flood
// that never gets past the transport exchange is unbounded.
TEST(SessionAdmission, WithAPoolEveryUnconnectedPeerCountsAgainstIt)
{
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Pool(4, 2, 0), Counts(0, 0, 2, 0)));
}

TEST(SessionAdmission, WithAPoolConnectedPeersDoNotCountAgainstIt)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Pool(100, 2, 0), Counts(99, 0, 1, 0)));
}

TEST(SessionAdmission, WithAPoolOneAddressIsBoundedByItsShare)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Pool(4, 10, 2), Counts(0, 1, 5, 1)));
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Pool(4, 10, 2), Counts(0, 2, 5, 2)));
}

TEST(SessionAdmission, WithAPoolAZeroShareMeansNoPerAddressBound)
{
	EXPECT_TRUE(SessionAdmission::MayStartConnecting(Pool(4, 10, 0), Counts(0, 9, 9, 9)));
}

TEST(SessionAdmission, WithAPoolThePoolBoundHoldsWhateverTheShare)
{
	EXPECT_FALSE(SessionAdmission::MayStartConnecting(Pool(4, 3, 10), Counts(0, 0, 3, 0)));
}

TEST(SessionAdmission, WithAPoolAcceptanceIsBoundedByTheIncomingLimit)
{
	EXPECT_TRUE(SessionAdmission::MayAccept(Pool(2, 8, 0), Counts(1, 5, 5, 0)));
	EXPECT_FALSE(SessionAdmission::MayAccept(Pool(2, 8, 0), Counts(2, 5, 5, 0)));
	EXPECT_FALSE(SessionAdmission::MayAccept(Pool(0, 8, 0), Counts(0, 1, 1, 0)));
}

// Differential check against a spelled-out model over a fixed-seed sweep, so a later rewrite of the
// rules cannot quietly change an edge nobody wrote a case for.
TEST(SessionAdmission, MatchesAReferenceModelOverRandomInputs)
{
	uint32_t state = 0x5E55104Du;
	const auto next = [&state](uint32_t bound) {
		state = state * 1664525u + 1013904223u;
		return (state >> 8) % bound;
	};

	for (int i = 0; i < 20000; ++i)
	{
		const SessionAdmission::Limits limits = Pool(next(6), next(3) == 0 ? 0 : next(8), next(4));
		SessionAdmission::Counts counts;
		counts.connected = next(8);
		counts.exchanging = next(6);
		counts.pending = counts.exchanging + next(6);
		counts.pendingFromAddress = next(counts.pending + 1);

		bool expectStart;
		bool expectAccept;
		if (limits.maxPending == 0)
		{
			expectStart = counts.connected + counts.exchanging < limits.maxIncoming;
			expectAccept = true;
		}
		else
		{
			const bool poolOk = counts.pending < limits.maxPending;
			const bool shareOk = limits.maxPendingPerAddress == 0 || counts.pendingFromAddress < limits.maxPendingPerAddress;
			expectStart = poolOk && shareOk;
			expectAccept = counts.connected < limits.maxIncoming;
		}

		ASSERT_EQ(SessionAdmission::MayStartConnecting(limits, counts), expectStart) << "case " << i;
		ASSERT_EQ(SessionAdmission::MayAccept(limits, counts), expectAccept) << "case " << i;
	}
}

// ---- the peer API is inert where it has nothing to act on --------------------------------------------------

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
	EXPECT_LT((int)ID_SESSION_CONFIG_STATUS, (int)ID_USER_PACKET_ENUM);
	EXPECT_LT((int)ID_SESSION_CONFIG_ABANDONED, (int)ID_USER_PACKET_ENUM);
}

// Both took reserved slots rather than inserting, so every id after them keeps its value and a peer built
// against the previous release still decodes everything else the same.
TEST(SessionAdmissionMessageIds, TookTheReservedSlotsWithoutShiftingLaterIds)
{
	EXPECT_EQ((int)ID_SESSION_CONFIG_STATUS, (int)ID_SESSION_CONFIG_REJECTED + 1);
	EXPECT_EQ((int)ID_SESSION_CONFIG_ABANDONED, (int)ID_SESSION_CONFIG_REJECTED + 2);
	EXPECT_EQ((int)ID_RESERVED_8, (int)ID_SESSION_CONFIG_REJECTED + 3);
}

TEST(SessionAdmissionMessageIds, AreDistinctFromTheOtherHandshakeIds)
{
	const int ids[] = {ID_SESSION_CONFIG_REQUEST, ID_SESSION_CONFIG, ID_SESSION_CONFIG_REJECTED, ID_SESSION_CONFIG_STATUS, ID_SESSION_CONFIG_ABANDONED};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
		for (size_t j = i + 1; j < sizeof(ids) / sizeof(ids[0]); ++j)
			EXPECT_NE(ids[i], ids[j]);
}
