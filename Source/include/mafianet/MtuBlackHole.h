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
/// (RakPeer.cpp pads ID_OPEN_CONNECTION_REQUEST_1 down the ladder) and the
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

#include "mafianet/Export.h"

namespace MafiaNet {

/// The MTU rung ladder, in bytes including UDP/IP headers, highest first.
/// The same rungs the connection handshake probes (mtuSizes in RakPeer.cpp);
/// MTU_LADDER[0] must equal MAXIMUM_MTU_SIZE so a step-down never lands on a
/// rung the handshake could not have negotiated.
const int MTU_LADDER_SIZE = 4;
extern RAK_DLL_EXPORT const int MTU_LADDER[MTU_LADDER_SIZE];

/// How many unacked transmissions of a too-large packet it takes before the
/// connection's MTU steps down one rung. Resends are RTO-spaced with backoff,
/// so this represents several round-trip times of a specific packet failing
/// while the connection is otherwise alive -- ordinary loss does not
/// concentrate on one packet like that.
const uint32_t MTU_BLACKHOLE_RESEND_THRESHOLD = 4;

/// The ladder rung strictly below \a currentMtuBytes, or 0 when already at or
/// below the bottom rung.
int RAK_DLL_EXPORT NextLowerMtu(int currentMtuBytes);

/// Whether a reliable packet occupying \a requiredDatagramBytes on the wire
/// (datagram payload plus UDP/IP headers) that has gone unacked through
/// \a timesSent transmissions is evidence of an MTU black hole worth stepping
/// down for. False when the packet already fits the next rung down: resending
/// it at the same size after a step-down would change nothing on the wire, so
/// its failures indicate loss, not a black hole.
bool RAK_DLL_EXPORT ShouldStepDownMtu(uint32_t timesSent, int requiredDatagramBytes, int currentMtuBytes);

} // namespace MafiaNet

#endif // __MTU_BLACK_HOLE_H
