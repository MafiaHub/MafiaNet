/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "mafianet/crypto/noise.h"
#include <sodium.h>
#include <cassert>
#include <cstring>

namespace MafiaNet {

// HMAC-SHA512 with an arbitrary-length key (libsodium's one-shot crypto_auth_hmacsha512
// fixes the key at 32 bytes, so we use the multipart API which accepts any keylen).
static void hmac512(const unsigned char *key, size_t keyLen,
                    const unsigned char *data, size_t dataLen,
                    unsigned char out[64])
{
	static const unsigned char dummy = 0;
	if (data == nullptr) data = &dummy;
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
	static const unsigned char dummy = 0;
	if (data == nullptr) data = &dummy;
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
		static const unsigned char dummy = 0;
		const unsigned char *pt = plaintext ? plaintext : &dummy;
		unsigned char npub[12]; nonce96(n, npub);
		unsigned long long clen = 0;
		crypto_aead_chacha20poly1305_ietf_encrypt(
			out, &clen, pt, len, h, 64, nullptr, npub, k);
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
		static const unsigned char dummy = 0;
		const unsigned char *ct = ciphertext ? ciphertext : &dummy;
		unsigned char npub[12]; nonce96(n, npub);
		unsigned long long mlen = 0;
		if (crypto_aead_chacha20poly1305_ietf_decrypt(
				out, &mlen, nullptr, ct, ciphertextLen, h, 64, npub, k) != 0) {
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

static const char *kProtocol = "Noise_NK_25519_ChaChaPoly_SHA512";

NoiseHandshake::NoiseHandshake()
	: initiator(false), hasInjectedEph(false)
{
	memset(e_pub, 0, 32); memset(e_sec, 0, 32);
	memset(remoteEph, 0, 32); memset(rs, 0, 32);
	memset(s_pub, 0, 32); memset(s_sec, 0, 32);
	memset(sendKey, 0, 32); memset(recvKey, 0, 32);
	memset(injectedEphSec, 0, 32);
	// Defense in depth: never expose uninitialized symmetric state if a message
	// is (incorrectly) processed before Init{Initiator,Responder} ran.
	memset(&ss, 0, sizeof ss);
}

NoiseHandshake::~NoiseHandshake()
{
	sodium_memzero(e_sec, 32);
	sodium_memzero(s_sec, 32);
	sodium_memzero(sendKey, 32);
	sodium_memzero(recvKey, 32);
	sodium_memzero(injectedEphSec, 32);
	sodium_memzero(&ss, sizeof ss);
}

void NoiseHandshake::SetEphemeralForTesting(const unsigned char ephSec[32])
{
	memcpy(injectedEphSec, ephSec, 32);
	hasInjectedEph = true;
}

void NoiseHandshake::GenEphemeral()
{
	if (hasInjectedEph) {
		memcpy(e_sec, injectedEphSec, 32);
		crypto_scalarmult_base(e_pub, e_sec);   // derive pub (clamps internally)
	} else {
		crypto_box_keypair(e_pub, e_sec);       // random X25519 keypair
	}
}

void NoiseHandshake::InitInitiator(const unsigned char responderStaticPub[32])
{
	initiator = true;
	memcpy(rs, responderStaticPub, 32);
	ss.InitializeSymmetric(kProtocol);
	ss.MixHash(nullptr, 0);          // MixHash(prologue=empty) -- REQUIRED
	ss.MixHash(rs, 32);              // pre-message: responder static
}

void NoiseHandshake::InitResponder(const unsigned char staticPub[32],
                                   const unsigned char staticSec[32])
{
	initiator = false;
	memcpy(s_pub, staticPub, 32);
	memcpy(s_sec, staticSec, 32);
	ss.InitializeSymmetric(kProtocol);
	ss.MixHash(nullptr, 0);          // MixHash(prologue=empty) -- REQUIRED
	ss.MixHash(s_pub, 32);           // pre-message: own static
}

void NoiseHandshake::WriteMessageA(unsigned char out[48])
{
	assert(initiator);
	GenEphemeral();
	ss.MixHash(e_pub, 32);
	memcpy(out, e_pub, 32);
	unsigned char dh[32];
	// crypto_scalarmult returns -1 for low-order/degenerate points, leaving dh unusable.
	// The pinned responder static key (rs) is caller-controlled; reject it rather than
	// mixing an undefined buffer into the handshake. A degenerate rs cannot produce a
	// usable session, so failing here is the correct fail-closed behavior.
	if (crypto_scalarmult(dh, e_sec, rs) != 0)     // es = DH(e, rs)
		sodium_memzero(dh, sizeof dh);             // mix a defined all-zero value; handshake fails downstream at the AEAD tag
	ss.MixKey(dh, 32);
	sodium_memzero(dh, sizeof dh);
	ss.EncryptAndHash(nullptr, 0, out + 32);   // empty payload -> 16-byte tag
}

bool NoiseHandshake::ReadMessageA(const unsigned char in[48])
{
	assert(!initiator);
	memcpy(remoteEph, in, 32);
	ss.MixHash(remoteEph, 32);
	unsigned char dh[32];
	if (crypto_scalarmult(dh, s_sec, remoteEph) != 0) {   // es = DH(s, re)
		sodium_memzero(dh, sizeof dh);
		return false;
	}
	ss.MixKey(dh, 32);
	sodium_memzero(dh, sizeof dh);
	unsigned char pt[16]; size_t ptLen = 0;
	return ss.DecryptAndHash(in + 32, 16, pt, &ptLen);
}

void NoiseHandshake::WriteMessageB(unsigned char out[48])
{
	assert(!initiator);
	GenEphemeral();
	ss.MixHash(e_pub, 32);
	memcpy(out, e_pub, 32);
	unsigned char dh[32];
	crypto_scalarmult(dh, e_sec, remoteEph);   // ee = DH(e, re)
	ss.MixKey(dh, 32);
	sodium_memzero(dh, sizeof dh);
	ss.EncryptAndHash(nullptr, 0, out + 32);
	unsigned char k1[32], k2[32];
	ss.Split(k1, k2);
	memcpy(recvKey, k1, 32);                    // responder: recv=k1, send=k2
	memcpy(sendKey, k2, 32);
	sodium_memzero(k1, 32); sodium_memzero(k2, 32);
}

bool NoiseHandshake::ReadMessageB(const unsigned char in[48])
{
	assert(initiator);
	memcpy(remoteEph, in, 32);
	ss.MixHash(remoteEph, 32);
	unsigned char dh[32];
	if (crypto_scalarmult(dh, e_sec, remoteEph) != 0) {   // ee = DH(e, re)
		sodium_memzero(dh, sizeof dh);
		return false;
	}
	ss.MixKey(dh, 32);
	sodium_memzero(dh, sizeof dh);
	unsigned char pt[16]; size_t ptLen = 0;
	if (!ss.DecryptAndHash(in + 32, 16, pt, &ptLen)) return false;
	unsigned char k1[32], k2[32];
	ss.Split(k1, k2);
	memcpy(sendKey, k1, 32);                    // initiator: send=k1, recv=k2
	memcpy(recvKey, k2, 32);
	sodium_memzero(k1, 32); sodium_memzero(k2, 32);
	return true;
}

void NoiseHandshake::GetTransportKeys(unsigned char outSend[32], unsigned char outRecv[32]) const
{
	memcpy(outSend, sendKey, 32);
	memcpy(outRecv, recvKey, 32);
}

} // namespace MafiaNet
