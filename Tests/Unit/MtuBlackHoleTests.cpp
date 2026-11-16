/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hermetic unit tests for the in-session MTU black-hole step-down decision.
 *
 *  The connection handshake probes the path MTU in one direction only and the
 *  result is then applied to both directions for the life of the connection.
 *  A tunnel (OpenVPN, WireGuard, ...) whose *return* path carries less than the
 *  probed direction silently drops every datagram over its ceiling: the peer
 *  connects, then hangs on the first split payload while the reliability layer
 *  resends the same too-large datagram until the connection times out.
 *
 *  ShouldStepDownMtu is the decision that breaks that loop: a reliable packet
 *  that has gone unacked through enough transmissions, and that would actually
 *  shrink if the MTU dropped a rung, indicates a black hole rather than plain
 *  loss. The damaging direction is the false positive -- ordinary packet loss
 *  must never shrink a healthy connection's MTU, so packets that already fit
 *  the next rung down can never trigger a step-down no matter how often they
 *  are resent.
 */

#include <gtest/gtest.h>

#include "mafianet/mtu_black_hole.h"
#include "mafianet/mtu_size.h"

using namespace MafiaNet;

TEST(MtuBlackHole, LadderTopIsTheNegotiationCeiling)
{
	// The in-session ladder must start where the handshake ladder starts, or a
	// step-down could move to a rung the handshake would never have negotiated.
	EXPECT_EQ(MAXIMUM_MTU_SIZE, MTU_LADDER[0]);
}

TEST(MtuBlackHole, NextLowerMtuWalksTheLadder)
{
	EXPECT_EQ(1280, NextLowerMtu(1400));
	EXPECT_EQ(1024, NextLowerMtu(1280));
	EXPECT_EQ(576, NextLowerMtu(1024));
}

TEST(MtuBlackHole, NextLowerMtuStopsAtTheBottomRung)
{
	EXPECT_EQ(0, NextLowerMtu(576));
	EXPECT_EQ(0, NextLowerMtu(400));
}

TEST(MtuBlackHole, NextLowerMtuFromBetweenRungsPicksTheRungStrictlyBelow)
{
	// Defensive: the negotiated MTU is normally a ladder value, but a peer built
	// with a custom MAXIMUM_MTU_SIZE can negotiate anything up to the cap.
	EXPECT_EQ(1280, NextLowerMtu(1300));
	EXPECT_EQ(576, NextLowerMtu(1000));
}

TEST(MtuBlackHole, StepsDownWhenALargePacketExhaustsItsResendBudget)
{
	// A datagram that fills the negotiated 1400-byte MTU has failed
	// MTU_BLACKHOLE_RESEND_THRESHOLD times: this is the black-hole signature.
	EXPECT_TRUE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD, 1400, 1400));
}

TEST(MtuBlackHole, DoesNotStepDownBeforeTheResendBudgetIsExhausted)
{
	EXPECT_FALSE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD - 1, 1400, 1400));
}

TEST(MtuBlackHole, DoesNotStepDownForAPacketThatAlreadyFitsTheNextRung)
{
	// Resending a small packet at the same size after a step-down changes
	// nothing on the wire, so its failures say "loss", not "black hole".
	// This is the false positive that would shrink healthy connections under
	// ordinary packet loss.
	EXPECT_FALSE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD, 1280, 1400));
	EXPECT_FALSE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD * 10, 100, 1400));
}

TEST(MtuBlackHole, StepsDownForAPacketJustOverTheNextRung)
{
	EXPECT_FALSE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD, 1280, 1400));
	EXPECT_TRUE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD, 1281, 1400));
}

TEST(MtuBlackHole, DoesNotStepDownBelowTheBottomRung)
{
	EXPECT_FALSE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD * 10, 576, 576));
}

TEST(MtuBlackHole, StepsDownFromIntermediateRungs)
{
	EXPECT_TRUE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD, 1280, 1280));
	EXPECT_TRUE(ShouldStepDownMtu(MTU_BLACKHOLE_RESEND_THRESHOLD, 1024, 1024));
}

// MtuBlackHoleEvidence: the size-specific half of the signature (issue #94). Packet loss hits datagrams
// of every size, so on a lossy link some large datagrams still get through; a black hole drops them all.

namespace
{
// A failure in a resend round of its own: the transmission that failed went out after the previous
// round's failures were detected.
uint64_t g_round = 0;
void FailInANewRound(MtuBlackHoleEvidence &evidence, int requiredBytes, int mtu)
{
	++g_round;
	evidence.OnResend(requiredBytes, mtu, g_round * 1000, g_round * 1000 + 500);
}
} // namespace

TEST(MtuBlackHoleEvidence, StartsWithNoEvidence)
{
	MtuBlackHoleEvidence evidence;
	EXPECT_FALSE(evidence.IsBlackHoleLikely());
	EXPECT_EQ(0u, evidence.GetFailures());
}

TEST(MtuBlackHoleEvidence, ConsecutiveLargeFailuresMakeABlackHoleLikely)
{
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i + 1 < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD; i++)
		FailInANewRound(evidence, 1400, 1400);
	EXPECT_FALSE(evidence.IsBlackHoleLikely());
	FailInANewRound(evidence, 1400, 1400);
	EXPECT_TRUE(evidence.IsBlackHoleLikely());
}

TEST(MtuBlackHoleEvidence, FailuresOfPacketsThatFitTheNextRungAreNotCounted)
{
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD * 10; i++)
	{
		FailInANewRound(evidence, 1280, 1400);
		FailInANewRound(evidence, 100, 1400);
	}
	EXPECT_EQ(0u, evidence.GetFailures());
}

TEST(MtuBlackHoleEvidence, AtTheBottomRungNothingIsCounted)
{
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD; i++)
		FailInANewRound(evidence, 576, 576);
	EXPECT_FALSE(evidence.IsBlackHoleLikely());
}

TEST(MtuBlackHoleEvidence, AnAckOfALargeEnoughDatagramStartsTheCountOver)
{
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i + 1 < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD; i++)
		FailInANewRound(evidence, 1400, 1400);
	evidence.OnDatagramAcked(1400);
	EXPECT_EQ(0u, evidence.GetFailures());
	FailInANewRound(evidence, 1400, 1400);
	EXPECT_FALSE(evidence.IsBlackHoleLikely());
}

TEST(MtuBlackHoleEvidence, AnAckAsLargeAsTheSmallestFailureIsEnough)
{
	MtuBlackHoleEvidence evidence;
	FailInANewRound(evidence, 1400, 1400);
	FailInANewRound(evidence, 1300, 1400);
	evidence.OnDatagramAcked(1300);
	EXPECT_EQ(0u, evidence.GetFailures());
}

TEST(MtuBlackHoleEvidence, AnAckOfASmallerDatagramIsNoEvidenceAgainstABlackHole)
{
	// What a black hole looks like: small datagrams (acks, small messages) keep getting through.
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD; i++)
	{
		FailInANewRound(evidence, 1400, 1400);
		evidence.OnDatagramAcked(1299);
		evidence.OnDatagramAcked(60);
	}
	EXPECT_TRUE(evidence.IsBlackHoleLikely());
}

TEST(MtuBlackHoleEvidence, ResetDiscardsTheEvidence)
{
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD; i++)
		FailInANewRound(evidence, 1400, 1400);
	evidence.Reset();
	EXPECT_FALSE(evidence.IsBlackHoleLikely());
	// And the size floor with it: a small ack after a reset does not matter, a large failure counts.
	evidence.OnDatagramAcked(100);
	FailInANewRound(evidence, 1400, 1400);
	EXPECT_EQ(1u, evidence.GetFailures());
}

TEST(MtuBlackHoleEvidence, FailuresDetectedInOneRoundCountOnce)
{
	// A bulk transfer: many large packets went out together and their timeouts expire together. All of
	// them were sent before the first of these failures was detected, so they are one round.
	MtuBlackHoleEvidence evidence;
	for (uint32_t i = 0; i < MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD * 10; i++)
		evidence.OnResend(1400, 1400, 1000 + i, 5000 + i);
	EXPECT_EQ(1u, evidence.GetFailures());
	EXPECT_FALSE(evidence.IsBlackHoleLikely());
}

TEST(MtuBlackHoleEvidence, AResendOfARetransmissionIsANewRound)
{
	// A packet resent when the first failure was detected, and failing again, counts again: in a black
	// hole every round fails, so the count still climbs one per retransmission timeout.
	MtuBlackHoleEvidence evidence;
	uint64_t sent = 0;
	for (uint32_t round = 1; round <= MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD; round++)
	{
		const uint64_t detected = sent + 300;
		evidence.OnResend(1400, 1400, sent, detected);
		evidence.OnResend(1400, 1400, sent, detected); // a sibling sent with it
		sent = detected;                               // both resent now
	}
	EXPECT_EQ(MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD, evidence.GetFailures());
	EXPECT_TRUE(evidence.IsBlackHoleLikely());
}
