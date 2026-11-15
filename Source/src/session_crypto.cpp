/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/session_crypto.h"

#include "mafianet/bit_stream.h"
#include "mafianet/version.h"

#include <sodium.h>

#include <string.h>

namespace MafiaNet
{

namespace
{

constexpr int kKeyBytes = 32;
constexpr int kTagBytes = 16;
constexpr uint8_t kPrefixValidBit = 0x80;

static_assert(crypto_aead_chacha20poly1305_ietf_KEYBYTES == kKeyBytes, "key size");
static_assert(crypto_aead_chacha20poly1305_ietf_NPUBBYTES == 12, "nonce size");
static_assert(crypto_aead_chacha20poly1305_ietf_ABYTES == kTagBytes, "tag size");
static_assert(crypto_scalarmult_curve25519_BYTES == kKeyBytes, "X25519 size");
static_assert(crypto_scalarmult_curve25519_SCALARBYTES == kKeyBytes, "X25519 size");
static_assert(crypto_auth_hmacsha256_BYTES == kKeyBytes, "HMAC-SHA256 size");
static_assert(RAKNET_PROTOCOL_VERSION >= 0 && RAKNET_PROTOCOL_VERSION <= 255, "protocol version must fit the AAD byte");

void WriteLE64(uint8_t *out, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		out[i] = (uint8_t) (v >> (8 * i));
}

uint64_t ReadLE64(const uint8_t *in)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t) in[i] << (8 * i);
	return v;
}

// netcode_sequence_number_bytes_required: the count of low bytes needed, in [1,8].
int SequenceBytes(uint64_t sequence)
{
	int n = 1;
	while (n < 8 && (sequence >> (8 * n)) != 0)
		n++;
	return n;
}

const char kInfoLabel[] = "MafiaNet datagram";
constexpr size_t kInfoLabelBytes = sizeof(kInfoLabel) - 1; // no NUL
static_assert(kInfoLabelBytes == 17, "info label is 17 bytes");
// label | protocol version | guid I | guid R | crypt info I | crypt info R | direction
constexpr size_t kInfoBytes = kInfoLabelBytes + 1 + 8 + 8 + 2 * CryptInfo::kSerializedBytes + 1;

} // namespace

// ------------------------------------------------------------------ CryptInfo

void CryptInfo::SerializeTo(uint8_t out[kSerializedBytes]) const
{
	out[0] = (uint8_t) mode;
	memcpy(out + 1, publicKey, sizeof(publicKey));
	WriteLE64(out + 1 + sizeof(publicKey), nonce);
}

void CryptInfo::Serialize(BitStream *out) const
{
	uint8_t raw[kSerializedBytes];
	SerializeTo(raw);
	out->WriteAlignedBytes(raw, kSerializedBytes);
}

bool CryptInfo::Deserialize(BitStream *in)
{
	uint8_t raw[kSerializedBytes];
	if (!in->ReadAlignedBytes(raw, kSerializedBytes))
		return false;
	if (raw[0] != (uint8_t) EncryptionMode::Required && raw[0] != (uint8_t) EncryptionMode::Plaintext)
		return false;
	mode = (EncryptionMode) raw[0];
	memcpy(publicKey, raw + 1, sizeof(publicKey));
	nonce = ReadLE64(raw + 1 + sizeof(publicKey));
	return true;
}

// ------------------------------------------------------------ EphemeralKeyPair

EphemeralKeyPair::EphemeralKeyPair()
{
	// Idempotent and thread-safe; RakPeer::Startup has normally done it already.
	// GCC's warn_unused_result ignores a (void) cast, hence the named result.
	const int sodiumReady = sodium_init();
	(void) sodiumReady;
	randombytes_buf(secretKey, sizeof(secretKey));
	crypto_scalarmult_curve25519_base(publicKey, secretKey);
	randombytes_buf(&nonce, sizeof(nonce));
	wiped = false;
}

EphemeralKeyPair::~EphemeralKeyPair()
{
	Wipe();
}

EphemeralKeyPair::EphemeralKeyPair(const EphemeralKeyPair &other)
{
	memcpy(secretKey, other.secretKey, sizeof(secretKey));
	memcpy(publicKey, other.publicKey, sizeof(publicKey));
	nonce = other.nonce;
	wiped = other.wiped;
}

EphemeralKeyPair &EphemeralKeyPair::operator=(const EphemeralKeyPair &other)
{
	if (this != &other)
	{
		memcpy(secretKey, other.secretKey, sizeof(secretKey));
		memcpy(publicKey, other.publicKey, sizeof(publicKey));
		nonce = other.nonce;
		wiped = other.wiped;
	}
	return *this;
}

CryptInfo EphemeralKeyPair::MakeCryptInfo() const
{
	CryptInfo info;
	info.mode = EncryptionMode::Required;
	memcpy(info.publicKey, publicKey, sizeof(publicKey));
	info.nonce = nonce;
	return info;
}

const uint8_t *EphemeralKeyPair::SecretKey() const
{
	return secretKey;
}

void EphemeralKeyPair::Wipe()
{
	sodium_memzero(secretKey, sizeof(secretKey));
	sodium_memzero(publicKey, sizeof(publicKey));
	nonce = 0;
	wiped = true;
}

bool EphemeralKeyPair::IsWiped() const
{
	return wiped;
}

// ---------------------------------------------------------------- SessionKeys

SessionKeys::~SessionKeys()
{
	Wipe();
}

void SessionKeys::Wipe()
{
	sodium_memzero(send, sizeof(send));
	sodium_memzero(recv, sizeof(recv));
}

// ------------------------------------------------------------- key derivation

bool DeriveSessionKeys(EphemeralKeyPair &local, bool localIsInitiator, const CryptInfo &initiatorInfo, const CryptInfo &responderInfo, RakNetGUID initiatorGuid, RakNetGUID responderGuid, SessionKeys *out)
{
	if (out == nullptr)
	{
		local.Wipe();
		return false;
	}
	out->Wipe();

	const CryptInfo &remote = localIsInitiator ? responderInfo : initiatorInfo;
	if (local.IsWiped() || remote.mode != EncryptionMode::Required)
	{
		local.Wipe();
		return false;
	}

	// crypto_scalarmult fails when the result is all zero (a low-order remote point).
	uint8_t shared[kKeyBytes];
	const int dh = crypto_scalarmult_curve25519(shared, local.SecretKey(), remote.publicKey);
	local.Wipe();
	if (dh != 0)
	{
		sodium_memzero(shared, sizeof(shared));
		return false;
	}

	uint8_t salt[16];
	WriteLE64(salt, initiatorInfo.nonce);
	WriteLE64(salt + 8, responderInfo.nonce);
	uint8_t prk[kKeyBytes];
	detail::HkdfExtract(salt, sizeof(salt), shared, sizeof(shared), prk);
	sodium_memzero(shared, sizeof(shared));

	uint8_t info[kInfoBytes];
	size_t pos = 0;
	memcpy(info + pos, kInfoLabel, kInfoLabelBytes);
	pos += kInfoLabelBytes;
	info[pos++] = (uint8_t) RAKNET_PROTOCOL_VERSION;
	WriteLE64(info + pos, initiatorGuid.g);
	pos += 8;
	WriteLE64(info + pos, responderGuid.g);
	pos += 8;
	initiatorInfo.SerializeTo(info + pos);
	pos += CryptInfo::kSerializedBytes;
	responderInfo.SerializeTo(info + pos);
	pos += CryptInfo::kSerializedBytes;
	const size_t directionPos = pos++;

	uint8_t keyI2R[kKeyBytes];
	uint8_t keyR2I[kKeyBytes];
	info[directionPos] = 0x01;
	const bool ok1 = detail::HkdfExpand(prk, info, kInfoBytes, keyI2R, sizeof(keyI2R));
	info[directionPos] = 0x02;
	const bool ok2 = detail::HkdfExpand(prk, info, kInfoBytes, keyR2I, sizeof(keyR2I));
	sodium_memzero(prk, sizeof(prk));

	const bool ok = ok1 && ok2;
	if (ok)
	{
		memcpy(out->send, localIsInitiator ? keyI2R : keyR2I, kKeyBytes);
		memcpy(out->recv, localIsInitiator ? keyR2I : keyI2R, kKeyBytes);
	}
	sodium_memzero(keyI2R, sizeof(keyI2R));
	sodium_memzero(keyR2I, sizeof(keyR2I));
	return ok;
}

// --------------------------------------------------------------- ReplayWindow

ReplayWindow::ReplayWindow()
{
	Reset();
}

void ReplayWindow::Reset()
{
	mostRecent = 0;
	for (uint64_t &slot : received)
		slot = UINT64_MAX; // empty
}

bool ReplayWindow::AlreadyReceived(uint64_t sequence) const
{
	// The maximum value marks an empty slot, so it can never be accepted.
	if (sequence == UINT64_MAX)
		return true;

	// Written so it cannot overflow: "sequence + kSize <= mostRecent" wraps for
	// sequences near UINT64_MAX.
	if (mostRecent >= (uint64_t) kSize && sequence <= mostRecent - (uint64_t) kSize)
		return true;

	const uint64_t slot = received[sequence % (uint64_t) kSize];
	if (slot == UINT64_MAX)
		return false;
	return slot >= sequence;
}

void ReplayWindow::Advance(uint64_t sequence)
{
	if (sequence == UINT64_MAX)
		return;
	if (sequence > mostRecent)
		mostRecent = sequence;
	received[sequence % (uint64_t) kSize] = sequence;
}

// ------------------------------------------------------------- DatagramCipher

DatagramCipher::DatagramCipher()
{
	sodium_memzero(sendKey, sizeof(sendKey));
	sodium_memzero(recvKey, sizeof(recvKey));
	hasKeys = false;
	nextSendSequence = 0;
}

DatagramCipher::~DatagramCipher()
{
	Clear();
}

void DatagramCipher::SetKeys(const SessionKeys &keys)
{
	Clear();
	memcpy(sendKey, keys.send, sizeof(sendKey));
	memcpy(recvKey, keys.recv, sizeof(recvKey));
	hasKeys = true;
}

bool DatagramCipher::HasKeys() const
{
	return hasKeys;
}

bool DatagramCipher::GetKeysForTesting(SessionKeys *out) const
{
	if (!hasKeys)
	{
		out->Wipe();
		return false;
	}
	memcpy(out->send, sendKey, sizeof(out->send));
	memcpy(out->recv, recvKey, sizeof(out->recv));
	return true;
}

void DatagramCipher::Clear()
{
	sodium_memzero(sendKey, sizeof(sendKey));
	sodium_memzero(recvKey, sizeof(recvKey));
	hasKeys = false;
	nextSendSequence = 0;
	replay.Reset();
}

int DatagramCipher::Seal(const uint8_t *plaintext, int len, uint8_t *out, int outCapacity)
{
	if (!hasKeys || len < 0 || out == nullptr || (plaintext == nullptr && len > 0))
		return -1;
	const uint64_t sequence = nextSendSequence;
	if (sequence == UINT64_MAX) // reserved: never sent
		return -1;

	const int n = SequenceBytes(sequence);
	const int header = 1 + n;
	if (outCapacity < header + kTagBytes || len > outCapacity - header - kTagBytes)
		return -1;

	const uint8_t prefix = (uint8_t) (kPrefixValidBit | ((n - 1) << 4));
	out[0] = prefix;
	for (int i = 0; i < n; i++)
		out[1 + i] = (uint8_t) (sequence >> (8 * i));

	uint8_t nonce[12];
	detail::BuildNonce(sequence, nonce);
	const uint8_t aad[2] = {(uint8_t) RAKNET_PROTOCOL_VERSION, prefix};
	if (!detail::AeadSeal(sendKey, nonce, aad, sizeof(aad), plaintext, (size_t) len, out + header))
		return -1;

	// Consume the sequence only once the datagram exists, so a refused seal never
	// burns a nonce and a sealed one never repeats it.
	nextSendSequence = sequence + 1;
	return header + len + kTagBytes;
}

int DatagramCipher::Open(uint8_t *datagram, int len, uint8_t **plaintextOut)
{
	if (plaintextOut != nullptr)
		*plaintextOut = nullptr;
	if (!hasKeys || datagram == nullptr || plaintextOut == nullptr || len < 1)
		return -1;

	const uint8_t prefix = datagram[0];
	if ((prefix & kPrefixValidBit) == 0 || (prefix & 0x0F) != 0)
		return -1;
	const int n = ((prefix >> 4) & 0x07) + 1;
	if (len < 1 + n + kTagBytes)
		return -1;

	uint64_t sequence = 0;
	for (int i = 0; i < n; i++)
		sequence |= (uint64_t) datagram[1 + i] << (8 * i);
	if (sequence == UINT64_MAX)
		return -1;
	if (replay.AlreadyReceived(sequence))
		return -1;

	uint8_t nonce[12];
	detail::BuildNonce(sequence, nonce);
	const uint8_t aad[2] = {(uint8_t) RAKNET_PROTOCOL_VERSION, prefix};
	uint8_t *body = datagram + 1 + n;
	const int bodyLen = len - 1 - n;
	if (!detail::AeadOpen(recvKey, nonce, aad, sizeof(aad), body, (size_t) bodyLen, body))
		return -1;

	replay.Advance(sequence);
	*plaintextOut = body;
	return bodyLen - kTagBytes;
}

uint64_t DatagramCipher::NextSendSequence() const
{
	return nextSendSequence;
}

void DatagramCipher::SetNextSendSequenceForTesting(uint64_t s)
{
	nextSendSequence = s;
}

// --------------------------------------------------------------------- detail

namespace detail
{

void HkdfExtract(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen, uint8_t prk[32])
{
	// PRK = HMAC-SHA256(key = salt, message = IKM)
	crypto_auth_hmacsha256_state st;
	crypto_auth_hmacsha256_init(&st, salt, saltLen);
	crypto_auth_hmacsha256_update(&st, ikm, ikmLen);
	crypto_auth_hmacsha256_final(&st, prk);
	sodium_memzero(&st, sizeof(st));
}

bool HkdfExpand(const uint8_t prk[32], const uint8_t *info, size_t infoLen, uint8_t *okm, size_t okmLen)
{
	constexpr size_t hashLen = crypto_auth_hmacsha256_BYTES;
	if (okmLen > 255 * hashLen)
		return false;

	// T(0) = empty; T(i) = HMAC-SHA256(PRK, T(i-1) | info | i); OKM = T(1) | T(2) | ...
	uint8_t t[hashLen] = {};
	size_t tLen = 0;
	size_t done = 0;
	for (uint8_t counter = 1; done < okmLen; counter++)
	{
		crypto_auth_hmacsha256_state st;
		crypto_auth_hmacsha256_init(&st, prk, hashLen);
		crypto_auth_hmacsha256_update(&st, t, tLen);
		crypto_auth_hmacsha256_update(&st, info, infoLen);
		crypto_auth_hmacsha256_update(&st, &counter, 1);
		crypto_auth_hmacsha256_final(&st, t);
		sodium_memzero(&st, sizeof(st));
		tLen = hashLen;

		const size_t take = okmLen - done < hashLen ? okmLen - done : hashLen;
		memcpy(okm + done, t, take);
		done += take;
	}
	sodium_memzero(t, sizeof(t));
	return true;
}

void BuildNonce(uint64_t sequence, uint8_t nonce[12])
{
	memset(nonce, 0, 4);
	WriteLE64(nonce + 4, sequence);
}

bool AeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadLen, const uint8_t *pt, size_t ptLen, uint8_t *out)
{
	unsigned long long outLen = 0;
	const int rc = crypto_aead_chacha20poly1305_ietf_encrypt(out, &outLen, pt, (unsigned long long) ptLen, aad, (unsigned long long) aadLen, nullptr, nonce, key);
	return rc == 0 && outLen == (unsigned long long) ptLen + kTagBytes;
}

bool AeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadLen, const uint8_t *ct, size_t ctLen, uint8_t *out)
{
	if (ctLen < (size_t) kTagBytes)
		return false;
	unsigned long long outLen = 0;
	const int rc = crypto_aead_chacha20poly1305_ietf_decrypt(out, &outLen, nullptr, ct, (unsigned long long) ctLen, aad, (unsigned long long) aadLen, nonce, key);
	return rc == 0 && outLen == (unsigned long long) ctLen - kTagBytes;
}

} // namespace detail

} // namespace MafiaNet
