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

#include "mafianet/MtuBlackHole.h"
#include "mafianet/MTUSize.h"

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
