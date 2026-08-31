/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hermetic unit tests for the socket-error classification the connection
 *  handshake's MTU probe depends on, plus the bound on MAXIMUM_MTU_SIZE.
 *
 *  RNS2_IsDatagramTooLargeError decides whether a failed send means "this
 *  datagram is bigger than the local interface will carry" (abandon this MTU
 *  rung immediately) or something else (keep trying at this size). Getting it
 *  wrong is invisible in a build: a false negative only makes connecting
 *  slower, which is how the bug it replaces survived -- the old code compared
 *  Send()'s return value against 10040, a value sendto never returns.
 */

#include <gtest/gtest.h>

#include "mafianet/socket2.h"
#include "mafianet/MTUSize.h"

#include <errno.h>

using namespace MafiaNet;

TEST(SocketError, DatagramTooLargeIsRecognised)
{
#ifdef _WIN32
	EXPECT_TRUE(RNS2_IsDatagramTooLargeError(WSAEMSGSIZE));
#else
	EXPECT_TRUE(RNS2_IsDatagramTooLargeError(EMSGSIZE));
#endif
}

TEST(SocketError, WSAEMSGSIZEHasTheValueTheOldComparisonUsed)
{
	// The replaced code tested `Send(...) == 10040` -- the numeric value of
	// WSAEMSGSIZE -- against sendto's return value instead of against the error
	// code. Pin the constant to show the classification covers what that
	// comparison was reaching for: the fix is where the value is read from, not
	// what the value is.
#ifdef _WIN32
	EXPECT_EQ(10040, WSAEMSGSIZE);
	EXPECT_TRUE(RNS2_IsDatagramTooLargeError(10040));
#else
	GTEST_SKIP() << "WSAEMSGSIZE is a Winsock constant";
#endif
}

TEST(SocketError, NoErrorIsNotDatagramTooLarge)
{
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(0));
}

TEST(SocketError, TransientErrorsAreNotDatagramTooLarge)
{
	// A false positive here is the damaging direction: it would step the MTU
	// down over a socket that was merely busy for a moment, and the connection
	// would settle on a smaller datagram than the path can carry for its whole
	// lifetime.
#ifdef _WIN32
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(WSAEWOULDBLOCK));
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(WSAENOBUFS));
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(WSAECONNRESET));
#else
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(EWOULDBLOCK));
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(ENOBUFS));
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(EINTR));
#endif
}

TEST(SocketError, NegativeInputIsNotDatagramTooLarge)
{
	// A caller that reads the error slot when nothing actually failed must not
	// be told to shrink its datagrams.
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(-1));
#ifdef EMSGSIZE
	EXPECT_FALSE(RNS2_IsDatagramTooLargeError(-EMSGSIZE));
#endif
}

TEST(MTUSize, MaximumClearsCommonTunnelOverhead)
{
	// The handshake probes the path in one direction only and then applies the
	// result to both, with nothing detecting a black hole afterwards, so the top
	// rung has to be a size a tunnelled peer survives without discovering
	// anything. 1400 clears WireGuard (1420) and typical IPSec/IKEv2 (1400) on a
	// 1500-byte path. Raising it past 1420 reintroduces the connection failure
	// this bound exists to prevent.
	EXPECT_LE(MAXIMUM_MTU_SIZE, 1420);
	EXPECT_GT(MAXIMUM_MTU_SIZE, MINIMUM_MTU_SIZE);
}
