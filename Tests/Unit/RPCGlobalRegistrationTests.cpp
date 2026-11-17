/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/rpc.h"
#include "mafianet/peer.h"

#include <string>

using namespace MafiaNet;

namespace
{
void DummyRpc(MafiaNet::BitStream *, Packet *, void *) {}

// A newly constructed peer inherits the global registrations.
bool GlobalRegistrationLandedAs(const char *name)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	const bool registered = peer->GetRPC().UnregisterFunction(name);
	RakPeerInterface::DestroyInstance(peer);
	return registered;
}
}

TEST(RPCGlobalRegistration, PeerCreatedBeforeRegistrationAppliesItOnce)
{
	RakPeer peer;
	RPCGlobalRegistration registration("late", DummyRpc, nullptr);
	EXPECT_TRUE(peer.GetRPC().UnregisterFunction("late"));
	EXPECT_FALSE(peer.GetRPC().UnregisterFunction("late"));
}

// A name that exactly fills functionName minus the terminator must be stored
// verbatim. Before the fix, a max-length name left the buffer without a NUL,
// so the later strlen in RakString::Assign read past the array.
TEST(RPCGlobalRegistration, MaxLengthNameIsStoredNulTerminated)
{
	const std::string name(RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1, 'a');
	RPCGlobalRegistration reg(name.c_str(), DummyRpc, 0);
	EXPECT_TRUE(GlobalRegistrationLandedAs(name.c_str()));
}

// A name longer than the buffer must be clamped to what fits (plus NUL)
// rather than overflowing the array in release builds where RakAssert is a
// no-op.
TEST(RPCGlobalRegistration, OverlongNameIsTruncatedNotOverflowed)
{
	const std::string overlong(RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH + 5, 'b');
	const std::string truncated(RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1, 'b');
#ifdef _DEBUG
	// RakAssert fires on truncation in debug builds; only exercise the
	// release backstop when asserts are compiled out.
	GTEST_SKIP() << "truncation backstop is release-only; RakAssert fires in debug";
#else
	RPCGlobalRegistration reg(overlong.c_str(), DummyRpc, 0);
	EXPECT_TRUE(GlobalRegistrationLandedAs(truncated.c_str()));
#endif
}
