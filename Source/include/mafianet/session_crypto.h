/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file session_crypto.h
/// \brief Per-connection session crypto: ephemeral X25519 key agreement, the
/// HKDF-SHA256 key schedule, and ChaCha20-Poly1305 datagram protection with a
/// replay window.
///
/// The key schedule follows GameNetworkingSockets' BFinishCryptoHandshake; the
/// datagram format and replay window follow netcode (netcode_write_packet,
/// netcode_read_packet, netcode_replay_protection_*). This unit has no socket or
/// peer dependencies. Everything that touches key material wipes it with
/// sodium_memzero once it is no longer needed.
///
/// libsodium must be initialised (sodium_init) before use; RakPeer::Startup does
/// this, and EphemeralKeyPair's constructor does it defensively.

#ifndef __MAFIANET_SESSION_CRYPTO_H
#define __MAFIANET_SESSION_CRYPTO_H

#include "mafianet/export.h"
#include "mafianet/types.h"

#include <stddef.h>
#include <stdint.h>

namespace MafiaNet
{

class BitStream;

/// Encryption mode a peer announces in its crypt info.
enum class EncryptionMode : uint8_t
{
	Required = 0,
	Plaintext = 1
};

/// One side's handshake contribution, sent in ID_OPEN_CONNECTION_REQUEST_2 /
/// ID_OPEN_CONNECTION_REPLY_2.
struct RAK_DLL_EXPORT CryptInfo
{
	EncryptionMode mode = EncryptionMode::Required;
	/// X25519 public key; zero when mode == Plaintext.
	uint8_t publicKey[32] = {};
	/// Random per handshake; 0 when mode == Plaintext.
	uint64_t nonce = 0;

	static constexpr int kSerializedBytes = 1 + 32 + 8;

	/// Byte-aligned: mode, publicKey, nonce (little-endian).
	void Serialize(BitStream *out) const;
	/// \return false on a short read or an unknown mode.
	bool Deserialize(BitStream *in);
	/// Writes the same kSerializedBytes bytes Serialize writes.
	void SerializeTo(uint8_t out[kSerializedBytes]) const;
};

/// An ephemeral X25519 key pair and handshake nonce. The secret key is wiped by
/// Wipe(), by the destructor, and by DeriveSessionKeys.
class RAK_DLL_EXPORT EphemeralKeyPair
{
public:
	EphemeralKeyPair();
	~EphemeralKeyPair();
	EphemeralKeyPair(const EphemeralKeyPair &other);
	EphemeralKeyPair &operator=(const EphemeralKeyPair &other);

	/// Mode Required, this key pair's public key, and the nonce fixed at construction.
	CryptInfo MakeCryptInfo() const;
	const uint8_t *SecretKey() const;
	void Wipe();
	bool IsWiped() const;

private:
	uint8_t secretKey[32];
	uint8_t publicKey[32];
	uint64_t nonce;
	bool wiped;
};

/// Directional session keys. Wiped by the destructor.
struct RAK_DLL_EXPORT SessionKeys
{
	uint8_t send[32] = {};
	uint8_t recv[32] = {};
	~SessionKeys();
	void Wipe();
};

/// Derives the two directional session keys from an X25519 exchange, bound to the
/// full handshake transcript (both crypt infos, both GUIDs, the protocol version).
/// \a local is always wiped. Returns false (and zeroes \a out) on a low-order
/// remote public key, a remote that is not in mode Required, or a wiped \a local.
RAK_DLL_EXPORT bool DeriveSessionKeys(EphemeralKeyPair &local, bool localIsInitiator, const CryptInfo &initiatorInfo, const CryptInfo &responderInfo, RakNetGUID initiatorGuid, RakNetGUID responderGuid, SessionKeys *out);

/// netcode's replay protection: remembers the last kSize sequences received.
class RAK_DLL_EXPORT ReplayWindow
{
public:
	static constexpr int kSize = 256;

	ReplayWindow();
	void Reset();
	/// True for a replay, a sequence too old to track, or the reserved UINT64_MAX.
	bool AlreadyReceived(uint64_t sequence) const;
	/// Records \a sequence. Call only after the datagram decrypted successfully.
	void Advance(uint64_t sequence);

private:
	uint64_t mostRecent;
	uint64_t received[kSize];
};

/// Seals and opens connected datagrams:
/// `prefix | sequence (n bytes LE, n in [1,8]) | ciphertext | tag (16)`, where
/// prefix = 0x80 | ((n - 1) << 4). The nonce is 4 zero bytes followed by the
/// 64-bit little-endian sequence; the AAD is {RAKNET_PROTOCOL_VERSION, prefix}.
class RAK_DLL_EXPORT DatagramCipher
{
public:
	static constexpr int kMaxOverhead = 1 + 8 + 16; // 25

	DatagramCipher();
	~DatagramCipher();
	// Not copyable: two copies would seal with the same key and sequence (nonce reuse).
	DatagramCipher(const DatagramCipher &) = delete;
	DatagramCipher &operator=(const DatagramCipher &) = delete;

	/// Copies the keys and starts a fresh session (sequence 0, empty window).
	/// The caller wipes its own copy. Only ever pass freshly derived keys: the
	/// send sequence restarts at 0, so re-installing keys already used would reuse nonces.
	void SetKeys(const SessionKeys &keys);
	bool HasKeys() const;
	/// Wipes the keys and resets the send sequence and the replay window.
	void Clear();

	/// Seals plaintext[0..len) into \a out, which must not overlap \a plaintext.
	/// \return the sealed length, or -1 without consuming a sequence number when
	/// there are no keys, \a outCapacity is too small, or the send sequence is
	/// exhausted (UINT64_MAX is never sent).
	int Seal(const uint8_t *plaintext, int len, uint8_t *out, int outCapacity);
	/// Authenticates and decrypts \a datagram in place. Safe on arbitrary input.
	/// \return the plaintext length with *plaintextOut pointing into \a datagram,
	/// or -1 for anything malformed, replayed, too old or forged.
	int Open(uint8_t *datagram, int len, uint8_t **plaintextOut);

	uint64_t NextSendSequence() const;
	void SetNextSendSequenceForTesting(uint64_t s);

private:
	uint8_t sendKey[32];
	uint8_t recvKey[32];
	bool hasKeys;
	uint64_t nextSendSequence;
	ReplayWindow replay;
};

namespace detail
{
/// RFC 5869 HKDF-Extract with HMAC-SHA256.
RAK_DLL_EXPORT void HkdfExtract(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen, uint8_t prk[32]);
/// RFC 5869 HKDF-Expand with HMAC-SHA256. False if okmLen > 255 * 32.
RAK_DLL_EXPORT bool HkdfExpand(const uint8_t prk[32], const uint8_t *info, size_t infoLen, uint8_t *okm, size_t okmLen);
/// 4 zero bytes followed by the little-endian 64-bit sequence.
RAK_DLL_EXPORT void BuildNonce(uint64_t sequence, uint8_t nonce[12]);
/// crypto_aead_chacha20poly1305_ietf; writes ptLen + 16 bytes to \a out.
RAK_DLL_EXPORT bool AeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadLen, const uint8_t *pt, size_t ptLen, uint8_t *out);
/// Inverse of AeadSeal; writes ctLen - 16 bytes to \a out (which may equal \a ct).
RAK_DLL_EXPORT bool AeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadLen, const uint8_t *ct, size_t ctLen, uint8_t *out);
} // namespace detail

} // namespace MafiaNet

#endif // __MAFIANET_SESSION_CRYPTO_H
