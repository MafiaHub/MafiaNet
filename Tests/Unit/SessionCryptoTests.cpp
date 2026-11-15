/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <sodium.h>

#include "SimulatedNetwork.h"

using namespace MafiaNet;

TEST(SessionCrypto, SodiumIsInitializedAfterPeerStartup)
{
	SimulatedNetwork net;
	net.AddPeer(1);
	// sodium_init() returns 1 when the library was already initialised.
	EXPECT_EQ(sodium_init(), 1);
}
