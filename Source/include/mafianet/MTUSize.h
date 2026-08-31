/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

/// \file
/// \brief \b [Internal] Defines the default maximum transfer unit.
///


#ifndef MAXIMUM_MTU_SIZE

/// \li \em 17914 16 Mbit/Sec Token Ring
/// \li \em 4464 4 Mbits/Sec Token Ring
/// \li \em 4352 FDDI
/// \li \em 1500. The largest Ethernet packet size \b recommended. This is the typical setting for non-PPPoE, non-VPN connections. The default value for NETGEAR routers, adapters and switches.
/// \li \em 1492. The size PPPoE prefers.
/// \li \em 1472. Maximum size to use for pinging. (Bigger packets are fragmented.)
/// \li \em 1468. The size DHCP prefers.
/// \li \em 1460. Usable by AOL if you don't have large email attachments, etc.
/// \li \em 1430. The size VPN and PPTP prefer.
/// \li \em 1400. Maximum size for AOL DSL.
/// \li \em 1280. The IPv6 minimum MTU, and what WireGuard-derived tunnels commonly settle on.
/// \li \em 576. Typical value to connect to dial-up ISPs.
/// The largest value for an UDP datagram

/// The ceiling on a negotiated per-connection MTU, and the size of every
/// datagram buffer in the library.
///
/// This was 1492 (PPPoE) until it was found to black-hole tunnelled clients.
/// The handshake probes the path in one direction only -- the connecting peer
/// pads ID_OPEN_CONNECTION_REQUEST_1 down the mtuSizes ladder in RakPeer.cpp and
/// the accepting peer echoes back whatever size arrived -- and the result is
/// then frozen for the life of the connection and applied to BOTH directions.
/// Nothing detects a path-MTU black hole afterwards: a datagram too large for
/// the return path is resent at the same size until the connection times out.
///
/// So the top rung has to be a size that survives whatever encapsulation a peer
/// sits behind, without that peer having to discover it. 1400 clears WireGuard
/// (1420) and typical IPSec/IKEv2 (1400) tunnels on a 1500-byte path; anything
/// smaller is still found by the ladder. The cost is ~6% of payload per
/// datagram on a clean path, against a class of connection failure that looks
/// to the user like the server ignoring them.
///
/// Lowering this further is safe. RAISING it is not, on its own: two peers
/// converge on the smaller of their two caps only because the accepting side
/// clamps to its own MAXIMUM_MTU_SIZE before replying, and both sides clamp
/// what they are told (RakPeer::AssignSystemAddressToRemoteSystemList).
#define MAXIMUM_MTU_SIZE 1400


#define MINIMUM_MTU_SIZE 400

#endif
