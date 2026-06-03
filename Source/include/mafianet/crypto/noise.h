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

// HKDF using HMAC-SHA512 (Noise HASHLEN = 64).
// Produces `numOutputs` (2 or 3) outputs of 64 bytes each.
// out must point to numOutputs*64 bytes.
RAK_DLL_EXPORT void Noise_HKDF(const unsigned char chainingKey[64],
                               const unsigned char *ikm, size_t ikmLen,
                               int numOutputs, unsigned char *out);

// Noise SymmetricState specialized to ChaChaPoly + SHA512.
struct NoiseSymmetricState
{
	unsigned char ck[64];   // chaining key
	unsigned char h[64];    // handshake hash
	unsigned char k[32];    // cipher key (valid only when hasKey)
	uint64_t      n;        // cipher nonce
	bool          hasKey;

	RAK_DLL_EXPORT void InitializeSymmetric(const char *protocolName);
	RAK_DLL_EXPORT void MixKey(const unsigned char *ikm, size_t ikmLen);
	RAK_DLL_EXPORT void MixHash(const unsigned char *data, size_t len);

	// Encrypt plaintext, using h as AEAD AAD, append 16-byte tag, then MixHash(ciphertext).
	// out must hold len+16 bytes. Returns bytes written (len+16, or len if no key yet).
	RAK_DLL_EXPORT size_t EncryptAndHash(const unsigned char *plaintext, size_t len, unsigned char *out);

	// Inverse. ciphertextLen includes the 16-byte tag when hasKey.
	// out must hold ciphertextLen bytes. Returns true on success (tag valid).
	RAK_DLL_EXPORT bool DecryptAndHash(const unsigned char *ciphertext, size_t ciphertextLen,
	                                   unsigned char *out, size_t *outLen);

	// Split() -> two 32-byte transport keys derived from ck.
	RAK_DLL_EXPORT void Split(unsigned char k1[32], unsigned char k2[32]) const;
};

} // namespace MafiaNet
