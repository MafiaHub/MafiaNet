/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "mafianet/crypto/securesession.h"
#include "mafianet/MTUSize.h"
#include <sodium.h>
#include <cstring>

namespace MafiaNet {

// Scratch must hold any datagram's ciphertext+tag. MafiaNet datagrams never exceed
// MAXIMUM_MTU_SIZE; add headroom for the AEAD tag and counter framing.
static const size_t SCRATCH_BYTES = MAXIMUM_MTU_SIZE + 64;
static_assert(SCRATCH_BYTES > MAXIMUM_MTU_SIZE, "scratch must exceed max datagram");

static void nonce96(uint64_t n, unsigned char out[12])
{
	memset(out, 0, 4);
	for (int i = 0; i < 8; ++i) out[4 + i] = (unsigned char)((n >> (8 * i)) & 0xff);
}
static uint64_t readLE64(const unsigned char *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
	return v;
}
static void writeLE64(unsigned char *p, uint64_t v)
{
	for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

SecureSession::SecureSession()
	: valid(false), txCounter(0), rxHighest(0), rxBitmap(0)
{
	memset(txKey, 0, 32);
	memset(rxKey, 0, 32);
}

SecureSession::~SecureSession()
{
	sodium_memzero(txKey, 32);
	sodium_memzero(rxKey, 32);
}

void SecureSession::SetKeys(const unsigned char sendKey[32], const unsigned char recvKey[32])
{
	memcpy(txKey, sendKey, 32);
	memcpy(rxKey, recvKey, 32);
	txCounter = 0;
	rxHighest = 0;
	rxBitmap = 0;
	valid = true;
}

bool SecureSession::Encrypt(unsigned char *buffer, size_t bufferSize, unsigned int &length)
{
	if (!valid) return false;
	// Counter exhaustion guard: never reuse a nonce. (Unreachable in practice; explicit.)
	if (txCounter == UINT64_MAX) return false;
	if ((size_t)length + OVERHEAD_BYTES > bufferSize) return false;

	uint64_t ctr = txCounter++;
	unsigned char npub[12]; nonce96(ctr, npub);

	unsigned char tmp[SCRATCH_BYTES];
	if ((size_t)length + 16 > sizeof tmp) return false;
	unsigned long long clen = 0;
	if (crypto_aead_chacha20poly1305_ietf_encrypt(
			tmp, &clen, buffer, length, nullptr, 0, nullptr, npub, txKey) != 0)
		return false; // fail closed; do not emit an unencrypted/partial datagram

	writeLE64(buffer, ctr);
	memcpy(buffer + 8, tmp, (size_t)clen);
	length = (unsigned int)(8 + clen);
	return true;
}

bool SecureSession::Decrypt(unsigned char *buffer, unsigned int &length)
{
	if (!valid) return false;
	if (length < OVERHEAD_BYTES) return false;   // counter + minimum tag

	uint64_t ctr = readLE64(buffer);
	unsigned char npub[12]; nonce96(ctr, npub);

	unsigned char tmp[SCRATCH_BYTES];
	size_t ctLen = length - 8;
	if (ctLen > sizeof tmp) return false;
	unsigned long long mlen = 0;
	if (crypto_aead_chacha20poly1305_ietf_decrypt(
			tmp, &mlen, nullptr, buffer + 8, ctLen, nullptr, 0, npub, rxKey) != 0) {
		return false;   // auth failure
	}
	if (!CheckAndUpdateReplay(ctr)) return false;

	if (mlen) memcpy(buffer, tmp, (size_t)mlen);
	length = (unsigned int)mlen;
	return true;
}

// IPsec/DTLS-style 64-entry anti-replay window. Called only AFTER a successful
// AEAD decrypt, so forged counters can't poison the window.
bool SecureSession::CheckAndUpdateReplay(uint64_t counter)
{
	const uint64_t W = 64;
	if (counter > rxHighest) {
		uint64_t shift = counter - rxHighest;
		rxBitmap = (shift >= W) ? 0 : (rxBitmap << shift);
		rxBitmap |= 1ull;
		rxHighest = counter;
		return true;
	}
	uint64_t diff = rxHighest - counter;
	if (diff >= W) return false;     // too old
	uint64_t mask = 1ull << diff;
	if (rxBitmap & mask) return false; // replay
	rxBitmap |= mask;
	return true;
}

} // namespace MafiaNet
