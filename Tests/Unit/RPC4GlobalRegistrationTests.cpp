/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/RPC4Plugin.h"
#include "mafianet/peerinterface.h"

#include <string>

using namespace MafiaNet;

namespace
{
	void DummyRpc(MafiaNet::BitStream *, Packet *, void *) {}

	// Attaches an RPC4 plugin (which synchronously replays every
	// RPC4GlobalRegistration made so far via OnAttach) and answers whether a
	// non-blocking function ended up registered under the given name.
	bool GlobalRegistrationLandedAs(const char *name)
	{
		RakPeerInterface *peer = RakPeerInterface::GetInstance();
		RPC4 rpc4;
		peer->AttachPlugin(&rpc4);
		const bool registered = rpc4.UnregisterFunction(name);
		peer->DetachPlugin(&rpc4);
		RakPeerInterface::DestroyInstance(peer);
		return registered;
	}
}

// A name that exactly fills functionName minus the terminator must be stored
// verbatim. Before the fix, a max-length name left the buffer without a NUL,
// so the later strlen in RakString::Assign read past the array.
TEST(RPC4GlobalRegistration, MaxLengthNameIsStoredNulTerminated)
{
	const std::string name(RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1, 'a');
	RPC4GlobalRegistration reg(name.c_str(), DummyRpc, 0);
	EXPECT_TRUE(GlobalRegistrationLandedAs(name.c_str()));
}

// A name longer than the buffer must be clamped to what fits (plus NUL)
// rather than overflowing the array in release builds where RakAssert is a
// no-op.
TEST(RPC4GlobalRegistration, OverlongNameIsTruncatedNotOverflowed)
{
	const std::string overlong(RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH + 5, 'b');
	const std::string truncated(RPC4_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH - 1, 'b');
#ifdef _DEBUG
	// RakAssert fires on truncation in debug builds; only exercise the
	// release backstop when asserts are compiled out.
	GTEST_SKIP() << "truncation backstop is release-only; RakAssert fires in debug";
#else
	RPC4GlobalRegistration reg(overlong.c_str(), DummyRpc, 0);
	EXPECT_TRUE(GlobalRegistrationLandedAs(truncated.c_str()));
#endif
}
