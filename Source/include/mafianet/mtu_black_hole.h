/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief In-session MTU black-hole detection.
///
/// The connection handshake probes the path MTU in one direction only
/// (peer.cpp pads ID_OPEN_CONNECTION_REQUEST_1 down the ladder) and the
/// negotiated size is then applied to both directions for the life of the
/// connection. A tunnel whose return path carries less than the probed
/// direction -- OpenVPN and friends drop, rather than fragment, datagrams over
/// their ceiling -- black-holes every large datagram one way while the
/// handshake's small packets sail through. The connection establishes, then
/// hangs on the first split payload.
///
/// These helpers are the portable decision logic the reliability layer uses to
/// break that loop: recognise the black-hole signature on a resent packet and
/// pick the next rung to drop to. Pure functions, no I/O, unit tested on every
/// platform (Tests/Unit/MtuBlackHoleTests.cpp).

#ifndef __MTU_BLACK_HOLE_H
#define __MTU_BLACK_HOLE_H

#include <stdint.h>

#include "mafianet/export.h"

namespace MafiaNet
{

/// The MTU rung ladder, in bytes including UDP/IP headers, highest first.
/// The same rungs the connection handshake probes (mtuSizes in peer.cpp);
/// MTU_LADDER[0] must equal MAXIMUM_MTU_SIZE so a step-down never lands on a
/// rung the handshake could not have negotiated.
const int MTU_LADDER_SIZE = 4;
extern MAFIANET_DATA_API const int MTU_LADDER[MTU_LADDER_SIZE];

/// How many unacked transmissions of a too-large packet it takes before the
/// connection's MTU steps down one rung. Resends are RTO-spaced with backoff,
/// so this represents several round-trip times of a specific packet failing
/// while the connection is otherwise alive -- ordinary loss does not
/// concentrate on one packet like that.
const uint32_t MTU_BLACKHOLE_RESEND_THRESHOLD = 4;

/// The ladder rung strictly below \a currentMtuBytes, or 0 when already at or
/// below the bottom rung.
int RAK_DLL_EXPORT NextLowerMtu(int currentMtuBytes);

/// How many transmissions of too-large packets must fail in a row -- each one
/// resent because its previous transmission went unacked, with no datagram of
/// that size acknowledged in between -- before a step-down. Packet loss hits
/// datagrams of every size, so on a lossy link some large datagrams still get
/// through and each one restarts the count; a black hole drops every one of
/// them. With loss p each way a transmission fails with probability
/// 1-(1-p)^2, so twelve failures in a row by chance take about 5e-6 at 20%
/// loss and 2e-8 at 10%, where the single-packet threshold above alone fired
/// for about 1 large packet in 60 at 20% loss (issue #94). A message with
/// several large fragments in flight reaches the count in as many resend
/// rounds as before; a lone large packet needs this many resends.
const uint32_t MTU_BLACKHOLE_LARGE_FAILURE_THRESHOLD = 12;

/// The size-specific half of the black-hole signature: failed transmissions of
/// datagrams larger than the next rung, counted until a datagram at least as
/// large as the smallest of them is acknowledged. Portable state, no I/O; the
/// reliability layer feeds it every resend and every acknowledged datagram.
class RAK_DLL_EXPORT MtuBlackHoleEvidence
{
public:
	MtuBlackHoleEvidence();

	/// A reliable packet that needs \a requiredDatagramBytes on the wire is
	/// being resent: its previous transmission was not acknowledged. Counted
	/// only when it would shrink at the next rung below \a currentMtuBytes.
	void OnResend(int requiredDatagramBytes, int currentMtuBytes);

	/// A datagram of \a datagramBytes on the wire was acknowledged. One at
	/// least as large as a counted failure shows the path carries that size,
	/// so the failures were loss, not size, and the count starts over.
	void OnDatagramAcked(int datagramBytes);

	/// Start over, e.g. after a step-down: the evidence was about the old size.
	void Reset();

	/// Whether enough large transmissions have failed in a row.
	bool IsBlackHoleLikely() const;

	uint32_t GetFailures() const { return failures; }

private:
	uint32_t failures;
	// The smallest required size among the counted failures.
	int smallestFailedBytes;
};

/// The per-packet half of the black-hole signature, checked together with
/// MtuBlackHoleEvidence::IsBlackHoleLikely().
/// Whether a reliable packet occupying \a requiredDatagramBytes on the wire
/// (datagram payload plus UDP/IP headers) that has gone unacked through
/// \a timesSent transmissions is evidence of an MTU black hole worth stepping
/// down for. False when the packet already fits the next rung down: resending
/// it at the same size after a step-down would change nothing on the wire, so
/// its failures indicate loss, not a black hole.
bool RAK_DLL_EXPORT ShouldStepDownMtu(uint32_t timesSent, int requiredDatagramBytes, int currentMtuBytes);

} // namespace MafiaNet

#endif // __MTU_BLACK_HOLE_H
