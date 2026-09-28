/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/TwoWayAuthentication.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/peerinterface.h"

using namespace MafiaNet;

namespace
{
	// The oldest and newest generation time: nonces generated back to back can straddle a
	// millisecond, so assertions are phrased against both ends.
	void NonceTimes(const TwoWayAuthentication::NonceGenerator &generator, Time &oldest, Time &newest)
	{
		oldest = generator.generatedNonces[0]->whenGenerated;
		newest = generator.generatedNonces[generator.generatedNonces.Size() - 1]->whenGenerated;
	}

	void GenerateNonces(TwoWayAuthentication::NonceGenerator &generator, unsigned int count)
	{
		char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
		unsigned short requestId;
		for (unsigned int i = 0; i < count; i++)
			generator.GetNonce(nonce, &requestId, AddressOrGUID(RakNetGUID(100 + i)));
	}
}

TEST(TwoWayAuthentication, TimeoutDefaultsAndRoundTrips)
{
	TwoWayAuthentication auth;
	EXPECT_EQ(auth.GetTimeout(), static_cast<Time>(TWO_WAY_AUTHENTICATION_DEFAULT_TIMEOUT_MS));
	auth.SetTimeout(30000);
	EXPECT_EQ(auth.GetTimeout(), static_cast<Time>(30000));
}

TEST(TwoWayAuthentication, NonceSurvivesUntilItsLifetimeEnds)
{
	TwoWayAuthentication::NonceGenerator generator;
	GenerateNonces(generator, 3);
	Time oldest, newest;
	NonceTimes(generator, oldest, newest);

	generator.Update(oldest + 30000 - 1, 30000);
	EXPECT_EQ(generator.generatedNonces.Size(), 3u);
}

TEST(TwoWayAuthentication, EveryExpiredNonceGoesInOneUpdate)
{
	TwoWayAuthentication::NonceGenerator generator;
	GenerateNonces(generator, 3);
	Time oldest, newest;
	NonceTimes(generator, oldest, newest);

	// One update past the newest nonce's lifetime clears all of them, not one per call.
	generator.Update(newest + 30000, 30000);
	EXPECT_EQ(generator.generatedNonces.Size(), 0u);
}

TEST(TwoWayAuthentication, OnlyTheExpiredPrefixIsRemoved)
{
	TwoWayAuthentication::NonceGenerator generator;
	GenerateNonces(generator, 2);
	Time oldest, newest;
	NonceTimes(generator, oldest, newest);
	// Age the first nonce without touching the second.
	generator.generatedNonces[0]->whenGenerated = newest - 1000;
	generator.generatedNonces[1]->whenGenerated = newest;

	generator.Update(newest + 500, 1000);
	ASSERT_EQ(generator.generatedNonces.Size(), 1u);
	EXPECT_EQ(generator.generatedNonces[0]->whenGenerated, newest);
}

TEST(TwoWayAuthentication, ClockSteppingBackwardsExpiresNothing)
{
	TwoWayAuthentication::NonceGenerator generator;
	GenerateNonces(generator, 1);
	const Time generated = generator.generatedNonces[0]->whenGenerated;

	generator.Update(generated - 1, 0);
	EXPECT_EQ(generator.generatedNonces.Size(), 1u);
}

TEST(TwoWayAuthentication, ChallengeTimesOutExactlyAtTheConfiguredTimeout)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	SocketDescriptor socket(0, "127.0.0.1");
	ASSERT_EQ(peer->Startup(1, &socket, 1), RAKNET_STARTED);

	TwoWayAuthentication auth;
	// Long enough that the real-time Update inside Receive() cannot expire it first.
	auth.SetTimeout(60000);
	peer->AttachPlugin(&auth);
	ASSERT_TRUE(auth.AddPassword("build", "token"));
	ASSERT_TRUE(auth.Challenge("build", AddressOrGUID(RakNetGUID(42))));
	ASSERT_EQ(auth.outgoingChallenges.Size(), 1u);
	const Time challenged = auth.outgoingChallenges.Peek().time;

	auth.UpdateTimeouts(challenged + 60000 - 1);
	EXPECT_EQ(auth.outgoingChallenges.Size(), 1u);

	auth.UpdateTimeouts(challenged + 60000);
	EXPECT_EQ(auth.outgoingChallenges.Size(), 0u);

	bool timedOut = false;
	for (Packet *packet = peer->Receive(); packet; peer->DeallocatePacket(packet), packet = peer->Receive())
		timedOut = timedOut || packet->data[0] == ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT;
	EXPECT_TRUE(timedOut);

	peer->DetachPlugin(&auth);
	peer->Shutdown(0);
	RakPeerInterface::DestroyInstance(peer);
}
