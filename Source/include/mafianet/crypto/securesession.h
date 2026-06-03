/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#pragma once
#include "mafianet/Export.h"
#include <cstdint>
#include <cstddef>

namespace MafiaNet {

// Per-connection transport encryption. Replaces cat::AuthenticatedEncryption.
// Wire layout per datagram: [8-byte LE counter][ciphertext][16-byte Poly1305 tag].
class RAK_DLL_EXPORT SecureSession
{
public:
	// Bytes added to every datagram: 8 (counter) + 16 (tag).
	static const unsigned OVERHEAD_BYTES = 24;

	SecureSession();
	void SetKeys(const unsigned char sendKey[32], const unsigned char recvKey[32]);
	bool IsValid() const { return valid; }

	// Encrypt in place. `buffer` holds `length` plaintext bytes, capacity `bufferSize`.
	// On success writes framed [counter|ciphertext|tag] back into buffer, updates length,
	// returns true. Returns false on capacity overflow, counter exhaustion, or !valid.
	bool Encrypt(unsigned char *buffer, size_t bufferSize, unsigned int &length);

	// Decrypt a framed datagram in place. `length` is received size.
	// On success writes plaintext back into buffer, updates length, returns true.
	// Returns false on bad length, auth failure, or replay.
	bool Decrypt(unsigned char *buffer, unsigned int &length);

private:
	bool          valid;
	unsigned char txKey[32], rxKey[32];
	uint64_t      txCounter;
	uint64_t      rxHighest;
	uint64_t      rxBitmap;     // 64-entry replay window
	bool CheckAndUpdateReplay(uint64_t counter);
};

} // namespace MafiaNet
