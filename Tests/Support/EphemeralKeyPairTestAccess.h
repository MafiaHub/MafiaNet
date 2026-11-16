/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#pragma once

#include "mafianet/session_crypto.h"

namespace MafiaNet
{
/// Test-only access to EphemeralKeyPair internals, so a test can rebuild the same pair after
/// DeriveSessionKeys has wiped one. EphemeralKeyPair befriends this name; the definition lives only
/// here, so the shipped library exports no way to read or clone a secret key. Never use it for a
/// real handshake.
struct EphemeralKeyPairTestAccess
{
	/// A key pair with a caller-chosen secret key and nonce.
	static EphemeralKeyPair FromSecretKey(const uint8_t secretKey[32], uint64_t nonce)
	{
		return EphemeralKeyPair(secretKey, nonce);
	}
	static const uint8_t *SecretKey(const EphemeralKeyPair &pair) { return pair.SecretKey(); }
};
} // namespace MafiaNet
