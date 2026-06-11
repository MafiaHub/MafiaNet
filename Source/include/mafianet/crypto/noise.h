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

// Noise_NK_25519_ChaChaPoly_SHA512.
//   <- s          (responder static, pre-known to initiator)
//   -> e, es      (message A, written by initiator / read by responder)
//   <- e, ee      (message B, written by responder / read by initiator)
// Each message on the wire is exactly 48 bytes (32-byte ephemeral pubkey + 16-byte tag).
class RAK_DLL_EXPORT NoiseHandshake
{
public:
	static const size_t MESSAGE_BYTES = 48;
	static const size_t KEY_BYTES = 32;

	NoiseHandshake();
	~NoiseHandshake();   // zeroes secret material

	// Copyable on purpose: RakPeer trial-verifies message B on a copy so a stale or
	// forged REPLY_2 cannot corrupt the in-progress handshake state. All members are
	// plain arrays/PODs; each copy zeroes its own secrets on destruction.
	NoiseHandshake(const NoiseHandshake&) = default;
	NoiseHandshake& operator=(const NoiseHandshake&) = default;

	// Initiator (client): knows the responder's static public key.
	void InitInitiator(const unsigned char responderStaticPub[32]);
	// Responder (server): holds its own static keypair.
	void InitResponder(const unsigned char staticPub[32], const unsigned char staticSec[32]);

	// Initiator: write message A (48 bytes) into out.
	void WriteMessageA(unsigned char out[48]);
	// Responder: read message A. Returns false on auth failure.
	bool ReadMessageA(const unsigned char in[48]);
	// Responder: write message B (48 bytes); on return, transport keys are ready.
	void WriteMessageB(unsigned char out[48]);
	// Initiator: read message B. Returns false on auth failure; on success keys are ready.
	bool ReadMessageB(const unsigned char in[48]);

	// Valid after the final message. sendKey/recvKey are 32 bytes each.
	void GetTransportKeys(unsigned char sendKey[32], unsigned char recvKey[32]) const;

	// TEST-ONLY: force the next generated ephemeral to use this fixed secret key
	// (so handshakes are deterministic for known-answer vectors). Production code
	// never calls this; ephemerals are random otherwise.
	void SetEphemeralForTesting(const unsigned char ephSec[32]);

private:
	NoiseSymmetricState ss;
	bool          initiator;
	unsigned char e_pub[32], e_sec[32];   // local ephemeral
	unsigned char remoteEph[32];          // peer ephemeral (stashed between Read/Write)
	unsigned char rs[32];                 // responder static (initiator side)
	unsigned char s_pub[32], s_sec[32];   // local static (responder side)
	unsigned char sendKey[32], recvKey[32];
	bool          hasInjectedEph;
	unsigned char injectedEphSec[32];
	void GenEphemeral();                  // fills e_pub/e_sec (injected or random)
};

} // namespace MafiaNet
