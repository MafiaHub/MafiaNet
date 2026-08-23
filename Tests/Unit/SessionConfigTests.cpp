/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string.h>

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/defines.h"

using namespace MafiaNet;

namespace
{
	// RAII holder so a failed ASSERT_ still destroys the peer.
	class PeerFixture : public ::testing::Test
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

// A peer with no configured payload reports an empty one rather than a stale pointer.
TEST_F(PeerFixture, SessionConfigDefaultsToEmpty)
{
	char *data = 0;
	unsigned int length = 1234;
	peer->GetSessionConfig(&data, &length);
	EXPECT_EQ(length, 0u);
}

TEST_F(PeerFixture, SessionConfigRoundTrips)
{
	const char payload[] = "{\"season\":\"winter\"}";
	const unsigned int payloadLength = (unsigned int)strlen(payload);

	peer->SetSessionConfig(payload, payloadLength);

	char *data = 0;
	unsigned int length = 0;
	peer->GetSessionConfig(&data, &length);

	ASSERT_EQ(length, payloadLength);
	ASSERT_NE(data, nullptr);
	EXPECT_EQ(memcmp(data, payload, payloadLength), 0);
}

// Setting a payload twice replaces it rather than appending, so a peer that reconfigures
// between connects does not accumulate stale bytes.
TEST_F(PeerFixture, SessionConfigOverwritesPreviousPayload)
{
	peer->SetSessionConfig("first-and-longer", 16);
	peer->SetSessionConfig("second", 6);

	char *data = 0;
	unsigned int length = 0;
	peer->GetSessionConfig(&data, &length);

	ASSERT_EQ(length, 6u);
	EXPECT_EQ(memcmp(data, "second", 6), 0);
}

TEST_F(PeerFixture, SessionConfigClearsOnZeroLength)
{
	peer->SetSessionConfig("something", 9);
	peer->SetSessionConfig(0, 0);

	char *data = 0;
	unsigned int length = 0;
	peer->GetSessionConfig(&data, &length);
	EXPECT_EQ(length, 0u);
}

// The cap is enforced on the way in, so an application cannot stage a payload that the
// receiving peer would reject as a protocol violation.
//
// Passing an oversized payload is a programmer error, so RakAssert trips on it by design; the silent
// clamp is the backstop that assert hides. RakAssert is armed by _DEBUG specifically (see defines.h),
// so gate on that rather than on the absence of NDEBUG -- a build that defines neither still has
// RakAssert compiled out and can exercise the clamp.
TEST_F(PeerFixture, SessionConfigIsCappedAtMaximum)
{
#if defined(_DEBUG)
	GTEST_SKIP() << "RakAssert is armed by _DEBUG and fires on oversized input; the clamp is the non-_DEBUG backstop";
#else
	const unsigned int oversized = MAXIMUM_SESSION_CONFIG_SIZE + 512;
	char *big = new char[oversized];
	memset(big, 'x', oversized);

	peer->SetSessionConfig(big, oversized);

	char *data = 0;
	unsigned int length = 0;
	peer->GetSessionConfig(&data, &length);
	EXPECT_EQ(length, (unsigned int)MAXIMUM_SESSION_CONFIG_SIZE);

	delete[] big;
#endif
}

// An unknown system has no payload; the out-parameter is still written so callers can rely on it.
TEST_F(PeerFixture, RemoteSessionConfigIsEmptyForUnknownSystem)
{
	unsigned int length = 4321;
	const char *data = peer->GetRemoteSessionConfig(UNASSIGNED_SYSTEM_ADDRESS, &length);
	EXPECT_EQ(data, nullptr);
	EXPECT_EQ(length, 0u);
}

// Accept/Reject on a peer that never made a session request must be inert, not a crash or a
// stray message onto an unrelated connection.
TEST_F(PeerFixture, AcceptAndRejectAreNoOpsForUnknownSystem)
{
	peer->AcceptSession(UNASSIGNED_SYSTEM_ADDRESS, "payload", 7);
	peer->RejectSession(UNASSIGNED_SYSTEM_ADDRESS, "nope");
	SUCCEED();
}

TEST_F(PeerFixture, SessionConfigInteractiveTogglesWithoutStartup)
{
	peer->SetSessionConfigInteractive(true);
	peer->SetSessionConfigInteractive(false);
	SUCCEED();
}

// The three handshake ids must stay below ID_USER_PACKET_ENUM: they were carved out of the
// reserved block precisely so application enums that start at ID_USER_PACKET_ENUM do not shift.
TEST(SessionConfigMessageIds, AreBelowUserPacketEnum)
{
	EXPECT_LT((int)ID_SESSION_CONFIG_REQUEST, (int)ID_USER_PACKET_ENUM);
	EXPECT_LT((int)ID_SESSION_CONFIG, (int)ID_USER_PACKET_ENUM);
	EXPECT_LT((int)ID_SESSION_CONFIG_REJECTED, (int)ID_USER_PACKET_ENUM);
}

TEST(SessionConfigMessageIds, AreDistinct)
{
	EXPECT_NE((int)ID_SESSION_CONFIG_REQUEST, (int)ID_SESSION_CONFIG);
	EXPECT_NE((int)ID_SESSION_CONFIG, (int)ID_SESSION_CONFIG_REJECTED);
	EXPECT_NE((int)ID_SESSION_CONFIG_REQUEST, (int)ID_SESSION_CONFIG_REJECTED);
}
