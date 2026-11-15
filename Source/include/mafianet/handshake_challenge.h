/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file handshake_challenge.h
/// \brief Stateless handshake cookie, after GameNetworkingSockets' UDP challenge
/// (GenerateChallenge / GetChallengeTime).
///
/// A challenge is a keyed SipHash (crypto_shorthash) of {time bucket, port, IPv6
/// address} with the low 16 bits replaced by the time bucket. The server keeps
/// no per-client state: it recomputes the hash when the client echoes the
/// challenge back, which proves the client can receive at the address it claims.
///
/// libsodium must be initialised (sodium_init) before use; RakPeer::Startup does
/// this.

#ifndef __MAFIANET_HANDSHAKE_CHALLENGE_H
#define __MAFIANET_HANDSHAKE_CHALLENGE_H

#include "mafianet/export.h"
#include "mafianet/time.h"
#include "mafianet/types.h"

#include <stdint.h>

namespace MafiaNet
{

class RAK_DLL_EXPORT HandshakeChallenge
{
  public:
	/// Draws a random secret with randombytes_buf.
	HandshakeChallenge();
	/// Wipes the secret.
	~HandshakeChallenge();

	HandshakeChallenge(const HandshakeChallenge &) = delete;
	HandshakeChallenge &operator=(const HandshakeChallenge &) = delete;

	/// Replaces the secret with 16 caller-supplied bytes (crypto_shorthash_KEYBYTES).
	void SetSecretForTesting(const uint8_t secret[16]);

	/// One bucket is 2^20 microseconds (about 1.05 s); wraps every 18 hours.
	static uint16_t TimeBucket(TimeUS now)
	{
		return (uint16_t) (now >> 20);
	}

	/// \return The challenge for \a from at time \a now.
	uint64_t Generate(TimeUS now, const SystemAddress &from) const;

	/// \return true if \a challenge was generated for \a from within the last four
	/// seconds. Rejects anything else, in constant time for the hash comparison.
	bool Verify(uint64_t challenge, TimeUS now, const SystemAddress &from) const;

  private:
	uint64_t Hash(uint16_t timeBucket, const SystemAddress &from) const;

	uint8_t secret[16];
};

} // namespace MafiaNet

#endif
