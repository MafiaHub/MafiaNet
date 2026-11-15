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
	/// Random per handshake. A client's nonce is random in both modes (ID_OPEN_CONNECTION_REPLY_2 echoes it);
	/// a Plaintext server's is 0.
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
	// Not copyable: a copy would survive the wipe DeriveSessionKeys performs on the
	// original, and the same secret could then key a second handshake.
	EphemeralKeyPair(const EphemeralKeyPair &) = delete;
	EphemeralKeyPair &operator=(const EphemeralKeyPair &) = delete;

	/// Test-only: a key pair with a caller-chosen secret key and nonce, so a test can
	/// rebuild the same pair after DeriveSessionKeys has wiped one. Never use it for a
	/// real handshake.
	static EphemeralKeyPair FromSecretKeyForTesting(const uint8_t secretKey[32], uint64_t nonce);

	/// Mode Required, this key pair's public key, and the nonce fixed at construction.
	CryptInfo MakeCryptInfo() const;
	const uint8_t *SecretKey() const;
	void Wipe();
	bool IsWiped() const;

private:
	EphemeralKeyPair(const uint8_t secretKey[32], uint64_t nonce);

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

// ------------------------------------------------------------- server identity
//
// A server may hold a long-term Ed25519 identity key (libsodium crypto_sign_ed25519_*).
// It signs the handshake transcript in ID_OPEN_CONNECTION_REPLY_2, and a client given
// the server's public key (Connect's PublicKey*) refuses any reply not signed by it.
// GameNetworkingSockets signs its crypt info the same way (CMsgSteamDatagramSessionCryptInfoSigned).

constexpr int kIdentityPublicKeyBytes = 32;
constexpr int kIdentitySecretKeyBytes = 64;
constexpr int kIdentitySignatureBytes = 64;

/// Creates a new server identity key pair (crypto_sign_ed25519_keypair). The secret
/// key is libsodium's 64-byte form: 32-byte seed followed by the 32-byte public key.
/// Publish \a publicKey; keep \a secretKey private and wipe it when done.
RAK_DLL_EXPORT void GenerateIdentityKeyPair(uint8_t publicKey[kIdentityPublicKeyBytes], uint8_t secretKey[kIdentitySecretKeyBytes]);

/// Reads the public half of an identity secret key.
/// \return false, zeroing \a publicKey, when \a secretKey is not a well-formed
/// libsodium Ed25519 secret key (its last 32 bytes must be the public key of its seed).
RAK_DLL_EXPORT bool IdentityPublicKeyFromSecret(const uint8_t secretKey[kIdentitySecretKeyBytes], uint8_t publicKey[kIdentityPublicKeyBytes]);

/// sodium_memzero, for code that holds key material but does not include sodium.h.
RAK_DLL_EXPORT void SecureZero(void *data, size_t length);

/// The identity block that follows the server crypt info in ID_OPEN_CONNECTION_REPLY_2:
///
///     flag (u8)                    0 = no identity key, 1 = present; any other value is malformed
///     [identity public key] (32)   only when flag == 1
///     [signature] (64)             only when flag == 1: Ed25519 over the identity transcript
///
/// 1 byte when absent, 97 when present.
struct RAK_DLL_EXPORT IdentityBlock
{
	bool present = false;
	uint8_t publicKey[kIdentityPublicKeyBytes] = {};
	uint8_t signature[kIdentitySignatureBytes] = {};

	static constexpr int kAbsentBytes = 1;
	static constexpr int kPresentBytes = 1 + kIdentityPublicKeyBytes + kIdentitySignatureBytes; // 97
	static constexpr int kMaxSerializedBytes = kPresentBytes;

	/// 1 or 97.
	int SerializedBytes() const;
	void Serialize(BitStream *out) const;
	/// \return false on a short read or a flag other than 0 or 1.
	bool Deserialize(BitStream *in);
	/// Writes the same bytes Serialize writes. \return the count, SerializedBytes().
	int SerializeTo(uint8_t out[kMaxSerializedBytes]) const;
};

/// The bytes the server identity key signs, 107 in all:
///
///     "MafiaNet server identity"   24 bytes of ASCII, no terminator
///     protocol version              1 byte (RAKNET_PROTOCOL_VERSION)
///     client crypt info            41 bytes, CryptInfo::SerializeTo, as sent in REQUEST_2
///     server crypt info            41 bytes, CryptInfo::SerializeTo, as sent in REPLY_2
///
/// The client crypt info carries the client's fresh nonce (and, in mode Required, its
/// ephemeral public key), so a signature is good for one handshake only.
constexpr int kIdentityTranscriptBytes = 24 + 1 + 2 * CryptInfo::kSerializedBytes;
RAK_DLL_EXPORT void BuildIdentityTranscript(const CryptInfo &clientInfo, const CryptInfo &serverInfo, uint8_t out[kIdentityTranscriptBytes]);

/// Fills \a out with the server's public key and its signature (crypto_sign_ed25519_detached)
/// over BuildIdentityTranscript(clientInfo, serverInfo).
/// \return false, leaving \a out absent, if \a secretKey is malformed.
RAK_DLL_EXPORT bool SignServerIdentity(const uint8_t secretKey[kIdentitySecretKeyBytes], const CryptInfo &clientInfo, const CryptInfo &serverInfo, IdentityBlock *out);
/// The same, with the public half the caller already validated with IdentityPublicKeyFromSecret(), which is
/// not recomputed (RakPeer caches it in SetIdentityKey and signs every REPLY_2 with it).
RAK_DLL_EXPORT bool SignServerIdentity(const uint8_t secretKey[kIdentitySecretKeyBytes], const uint8_t publicKey[kIdentityPublicKeyBytes], const CryptInfo &clientInfo, const CryptInfo &serverInfo, IdentityBlock *out);

/// \return true when \a identity is present and its signature verifies
/// (crypto_sign_ed25519_verify_detached) under its own public key over
/// BuildIdentityTranscript(clientInfo, serverInfo). Whether that public key is the one
/// the client expects is the caller's check. Safe on arbitrary input.
RAK_DLL_EXPORT bool VerifyServerIdentity(const IdentityBlock &identity, const CryptInfo &clientInfo, const CryptInfo &serverInfo);

/// Derives the two directional session keys from an X25519 exchange, bound to the
/// full handshake transcript (both crypt infos, both GUIDs, the protocol version, and
/// the responder's identity block from ID_OPEN_CONNECTION_REPLY_2).
/// \a local is always wiped. Returns false (and zeroes \a out) on a low-order
/// remote public key, a remote that is not in mode Required, or a wiped \a local.
RAK_DLL_EXPORT bool DeriveSessionKeys(EphemeralKeyPair &local, bool localIsInitiator, const CryptInfo &initiatorInfo, const CryptInfo &responderInfo, RakNetGUID initiatorGuid, RakNetGUID responderGuid, const IdentityBlock &responderIdentity, SessionKeys *out);
/// The same, for a responder without an identity key (an absent identity block).
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
	/// Test-only: copies the current keys into \a out (zeroed when there are none).
	/// \return HasKeys().
	bool GetKeysForTesting(SessionKeys *out) const;

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
