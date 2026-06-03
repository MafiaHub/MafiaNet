/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "mafianet/crypto/noise.h"
#include <sodium.h>
#include <cstring>

namespace MafiaNet {

// HMAC-SHA512 with an arbitrary-length key (libsodium's one-shot crypto_auth_hmacsha512
// fixes the key at 32 bytes, so we use the multipart API which accepts any keylen).
static void hmac512(const unsigned char *key, size_t keyLen,
                    const unsigned char *data, size_t dataLen,
                    unsigned char out[64])
{
	crypto_auth_hmacsha512_state st;
	crypto_auth_hmacsha512_init(&st, key, keyLen);
	crypto_auth_hmacsha512_update(&st, data, dataLen);
	crypto_auth_hmacsha512_final(&st, out);
}

void Noise_HKDF(const unsigned char chainingKey[64],
                const unsigned char *ikm, size_t ikmLen,
                int numOutputs, unsigned char *out)
{
	unsigned char tempKey[64];
	hmac512(chainingKey, 64, ikm, ikmLen, tempKey);

	unsigned char prev[64];
	unsigned char buf[65];
	buf[0] = 0x01;
	hmac512(tempKey, 64, buf, 1, prev);
	memcpy(out, prev, 64);

	for (int i = 2; i <= numOutputs; ++i) {
		memcpy(buf, prev, 64);
		buf[64] = (unsigned char)i;
		hmac512(tempKey, 64, buf, 65, prev);
		memcpy(out + (i - 1) * 64, prev, 64);
	}
	sodium_memzero(tempKey, sizeof tempKey);
	sodium_memzero(prev, sizeof prev);
}

void NoiseSymmetricState::InitializeSymmetric(const char *protocolName)
{
	size_t len = strlen(protocolName);
	memset(h, 0, 64);
	if (len <= 64) {
		memcpy(h, protocolName, len);
	} else {
		crypto_hash_sha512(h, (const unsigned char *)protocolName, len);
	}
	memcpy(ck, h, 64);
	hasKey = false;
	n = 0;
}

void NoiseSymmetricState::MixKey(const unsigned char *ikm, size_t ikmLen)
{
	unsigned char out[128];
	Noise_HKDF(ck, ikm, ikmLen, 2, out);
	memcpy(ck, out, 64);
	memcpy(k, out + 64, 32);   // truncate 64->32 for ChaCha key
	hasKey = true;
	n = 0;
	sodium_memzero(out, sizeof out);
}

void NoiseSymmetricState::MixHash(const unsigned char *data, size_t len)
{
	crypto_hash_sha512_state st;
	crypto_hash_sha512_init(&st);
	crypto_hash_sha512_update(&st, h, 64);
	crypto_hash_sha512_update(&st, data, len);
	crypto_hash_sha512_final(&st, h);
}

static void nonce96(uint64_t n, unsigned char out[12])
{
	memset(out, 0, 4);
	for (int i = 0; i < 8; ++i) out[4 + i] = (unsigned char)((n >> (8 * i)) & 0xff);
}

size_t NoiseSymmetricState::EncryptAndHash(const unsigned char *plaintext, size_t len,
                                           unsigned char *out)
{
	size_t written;
	if (hasKey) {
		unsigned char npub[12]; nonce96(n, npub);
		unsigned long long clen = 0;
		crypto_aead_chacha20poly1305_ietf_encrypt(
			out, &clen, plaintext, len, h, 64, nullptr, npub, k);
		written = (size_t)clen;
		++n;
	} else {
		if (len) memcpy(out, plaintext, len);
		written = len;
	}
	MixHash(out, written);
	return written;
}

bool NoiseSymmetricState::DecryptAndHash(const unsigned char *ciphertext, size_t ciphertextLen,
                                         unsigned char *out, size_t *outLen)
{
	if (hasKey) {
		unsigned char npub[12]; nonce96(n, npub);
		unsigned long long mlen = 0;
		if (crypto_aead_chacha20poly1305_ietf_decrypt(
				out, &mlen, nullptr, ciphertext, ciphertextLen, h, 64, npub, k) != 0) {
			return false;
		}
		*outLen = (size_t)mlen;
		++n;
	} else {
		if (ciphertextLen) memcpy(out, ciphertext, ciphertextLen);
		*outLen = ciphertextLen;
	}
	MixHash(ciphertext, ciphertextLen);
	return true;
}

void NoiseSymmetricState::Split(unsigned char k1[32], unsigned char k2[32]) const
{
	unsigned char out[128];
	Noise_HKDF(ck, nullptr, 0, 2, out);
	memcpy(k1, out, 32);
	memcpy(k2, out + 64, 32);
	sodium_memzero(out, sizeof out);
}

} // namespace MafiaNet
