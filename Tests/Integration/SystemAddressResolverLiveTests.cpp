/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/guid_util.h"
#include <gtest/gtest.h>

#include "mafianet/types.h"

using namespace MafiaNet;

// SystemAddress parsing that goes through the operating system's resolver
// (getaddrinfo) for a host name. These depend on the host's resolver
// configuration, so they live here rather than in the hermetic unit suite.

TEST(SystemAddressResolverLive, InvalidHostNameReturnsFalse)
{
	SystemAddress sa;
	// RFC 6761: ".invalid" is guaranteed never to resolve.
	EXPECT_FALSE(sa.FromString("x.invalid"));
}

TEST(SystemAddressResolverLive, SetBinaryAddressHostnameResolvesToIpv4)
{
	// SetBinaryAddress is the IPv4-only legacy parser. "localhost" resolves to
	// ::1 first on many hosts (RFC 6724 order); the IPv4 answer must still win
	// rather than the call silently keeping a stale address.
	SystemAddress sa("10.9.8.7", 4321);
	ASSERT_TRUE(sa.SetBinaryAddress("localhost"));
	EXPECT_EQ(sa.GetIPVersion(), 4);
	EXPECT_EQ(MafiaNet::to_string(sa, false), "127.0.0.1");
}
