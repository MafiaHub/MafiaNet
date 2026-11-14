/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  An address family the host cannot use must surface as
 *  SOCKET_FAMILY_NOT_SUPPORTED, not as "port already in use". Hermetic: the
 *  unsupported-family paths fail inside getaddrinfo()/socket() before anything
 *  is sent, and the one successful Startup binds 127.0.0.1 without connecting.
 */

#include <gtest/gtest.h>

#include "mafianet/peer_interface.h"
#include "mafianet/socket2.h"

#ifndef _WIN32
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

using namespace MafiaNet;

TEST(SocketFamilyErrors, ResolverFamilyErrorsRecognised)
{
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedResolveError(EAI_FAMILY));
#ifdef EAI_ADDRFAMILY
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedResolveError(EAI_ADDRFAMILY));
#endif
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedResolveError(0));
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedResolveError(EAI_NONAME));
}

TEST(SocketFamilyErrors, SocketFamilyErrorsRecognised)
{
#ifdef _WIN32
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(WSAEAFNOSUPPORT));
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(WSAEPFNOSUPPORT));
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedSocketError(WSAEADDRINUSE));
#else
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(EAFNOSUPPORT));
#ifdef EPFNOSUPPORT
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(EPFNOSUPPORT));
#endif
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedSocketError(EADDRINUSE));
#endif
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedSocketError(0));
}

// The berkley bind itself, below Startup's up-front check: getaddrinfo rejects
// the family (EAI_FAMILY), which must not be reported as a bind failure.
TEST(SocketFamilyBind, ResolverFamilyErrorReportsFamilyNotSupported)
{
	RakNetSocket2 *socket = RakNetSocket2Allocator::AllocRNS2();
	ASSERT_NE(socket, nullptr);
	ASSERT_TRUE(socket->IsBerkleySocket());

	RNS2_BerkleyBindParameters bbp = {};
	bbp.port = 0;
	bbp.hostAddress = nullptr;
	bbp.addressFamily = AF_UNIX;
	bbp.type = SOCK_DGRAM;
	bbp.protocol = 0;
	bbp.nonBlockingSocket = false;
	bbp.setBroadcast = false;
	bbp.setIPHdrIncl = false;
	bbp.doNotFragment = false;
	bbp.pollingThreadPriority = 0;
	bbp.eventHandler = nullptr;

	const RNS2BindResult result = static_cast<RNS2_Berkley *>(socket)->Bind(&bbp, _FILE_AND_LINE_);
	RakNetSocket2Allocator::DeallocRNS2(socket);
	EXPECT_EQ(result, BR_FAMILY_NOT_SUPPORTED);
}

TEST(SocketFamilyStartup, UnsupportedFamilyRejectedThenNormalStartupWorks)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();

	SocketDescriptor bad(0, "127.0.0.1");
	bad.socketFamily = AF_UNIX;
	EXPECT_EQ(peer->Startup(1, &bad, 1), SOCKET_FAMILY_NOT_SUPPORTED);
	EXPECT_FALSE(peer->IsActive());

	SocketDescriptor good(0, "127.0.0.1");
	EXPECT_EQ(peer->Startup(1, &good, 1), RAKNET_STARTED);
	EXPECT_TRUE(peer->IsActive());
	EXPECT_NE(peer->GetMyBoundAddress().GetPort(), 0);

	peer->Shutdown(0);
	RakPeerInterface::DestroyInstance(peer);
}
