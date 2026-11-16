/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <sodium.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <type_traits>
#include <vector>

#include "SimulatedNetwork.h"
#include "mafianet/bit_stream.h"
#include "mafianet/session_crypto.h"
#include "mafianet/version.h"

using namespace MafiaNet;

namespace
{

std::vector<uint8_t> FromHex(const char *hex)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2)
	{
		const std::string byte(hex + i, 2);
		out.push_back((uint8_t) strtoul(byte.c_str(), nullptr, 16));
	}
	return out;
}

std::vector<uint8_t> Payload(int len)
{
	std::vector<uint8_t> p((size_t) len);
	for (int i = 0; i < len; i++)
		p[(size_t) i] = (uint8_t) (i * 31 + 7);
	return p;
}

int SequenceBytes(uint64_t sequence)
{
	int n = 1;
	while (n < 8 && (sequence >> (8 * n)) != 0)
		n++;
	return n;
}

const RakNetGUID kInitiatorGuid(0x1111222233334444ull);
const RakNetGUID kResponderGuid(0x5555666677778888ull);

// Runs a full key agreement between two fresh ephemeral key pairs.
struct KeyAgreement
{
	SessionKeys initiator;
	SessionKeys responder;

	KeyAgreement()
	{
		EphemeralKeyPair i;
		EphemeralKeyPair r;
		const CryptInfo infoI = i.MakeCryptInfo();
		const CryptInfo infoR = r.MakeCryptInfo();
		EXPECT_TRUE(DeriveSessionKeys(i, true, infoI, infoR, kInitiatorGuid, kResponderGuid, &initiator));
		EXPECT_TRUE(DeriveSessionKeys(r, false, infoI, infoR, kInitiatorGuid, kResponderGuid, &responder));
	}
};

// An initiator-side sender and a responder-side receiver sharing one session.
struct CipherPair
{
	KeyAgreement keys;
	DatagramCipher sender;
	DatagramCipher receiver;

	CipherPair()
	{
		sender.SetKeys(keys.initiator);
		receiver.SetKeys(keys.responder);
	}

	std::vector<uint8_t> Seal(const std::vector<uint8_t> &plaintext)
	{
		std::vector<uint8_t> out(plaintext.size() + DatagramCipher::kMaxOverhead);
		const int n = sender.Seal(plaintext.data(), (int) plaintext.size(), out.data(), (int) out.size());
		EXPECT_GT(n, 0);
		out.resize(n > 0 ? (size_t) n : 0);
		return out;
	}

	// Opens a copy, so the caller's datagram survives the in-place decrypt.
	int Open(std::vector<uint8_t> datagram, std::vector<uint8_t> *plaintext = nullptr)
	{
		uint8_t *pt = nullptr;
		const int n = receiver.Open(datagram.data(), (int) datagram.size(), &pt);
		if (n >= 0 && plaintext != nullptr)
			plaintext->assign(pt, pt + n);
		return n;
	}
};

// Builds a datagram by hand with the wire layout, so tests can encode sequences
// the sender refuses to emit.
std::vector<uint8_t> HandSeal(const uint8_t key[32], uint64_t sequence, const std::vector<uint8_t> &plaintext)
{
	const int n = SequenceBytes(sequence);
	const uint8_t prefix = (uint8_t) (0x80 | ((n - 1) << 4));
	std::vector<uint8_t> out(1 + (size_t) n + plaintext.size() + 16);
	out[0] = prefix;
	for (int i = 0; i < n; i++)
		out[1 + (size_t) i] = (uint8_t) (sequence >> (8 * i));
	uint8_t nonce[12];
	detail::BuildNonce(sequence, nonce);
	const uint8_t aad[2] = {(uint8_t) RAKNET_PROTOCOL_VERSION, prefix};
	EXPECT_TRUE(detail::AeadSeal(key, nonce, aad, sizeof(aad), plaintext.data(), plaintext.size(), out.data() + 1 + n));
	return out;
}

} // namespace

TEST(SessionCrypto, SodiumIsInitializedAfterPeerStartup)
{
	SimulatedNetwork net;
	net.AddPeer(1);
	// sodium_init() returns 1 when the library was already initialised.
	EXPECT_EQ(sodium_init(), 1);
}

// ---------------------------------------------------------------- key schedule

TEST(SessionCrypto, KeysAgreeBetweenInitiatorAndResponder)
{
	KeyAgreement k;
	EXPECT_EQ(0, memcmp(k.initiator.send, k.responder.recv, 32));
	EXPECT_EQ(0, memcmp(k.initiator.recv, k.responder.send, 32));
	EXPECT_NE(0, memcmp(k.initiator.send, k.initiator.recv, 32));
}

TEST(SessionCrypto, AnyTranscriptChangeChangesKeys)
{
	EphemeralKeyPair i;
	EphemeralKeyPair r;
	const CryptInfo infoI = i.MakeCryptInfo();
	const CryptInfo infoR = r.MakeCryptInfo();
	uint8_t secretI[32];
	memcpy(secretI, EphemeralKeyPairTestAccess::SecretKey(i), sizeof(secretI));

	// DeriveSessionKeys wipes the key pair it is given, so rebuild the same pair each time.
	auto derive = [&](const CryptInfo &a, const CryptInfo &b, RakNetGUID ga, RakNetGUID gb, SessionKeys *out)
	{
		EphemeralKeyPair local = EphemeralKeyPairTestAccess::FromSecretKey(secretI, infoI.nonce);
		return DeriveSessionKeys(local, true, a, b, ga, gb, out);
	};

	SessionKeys base;
	ASSERT_TRUE(derive(infoI, infoR, kInitiatorGuid, kResponderGuid, &base));

	auto expectDifferent = [&](const SessionKeys &k, const char *what)
	{
		EXPECT_NE(0, memcmp(base.send, k.send, 32)) << what;
		EXPECT_NE(0, memcmp(base.recv, k.recv, 32)) << what;
	};

	{
		CryptInfo changed = infoI;
		changed.nonce ^= 1;
		SessionKeys k;
		ASSERT_TRUE(derive(changed, infoR, kInitiatorGuid, kResponderGuid, &k));
		expectDifferent(k, "initiator nonce");
	}
	{
		CryptInfo changed = infoR;
		changed.nonce ^= 0x8000000000000000ull;
		SessionKeys k;
		ASSERT_TRUE(derive(infoI, changed, kInitiatorGuid, kResponderGuid, &k));
		expectDifferent(k, "responder nonce");
	}
	{
		SessionKeys k;
		ASSERT_TRUE(derive(infoI, infoR, RakNetGUID(kInitiatorGuid.g ^ 1), kResponderGuid, &k));
		expectDifferent(k, "initiator guid");
	}
	{
		SessionKeys k;
		ASSERT_TRUE(derive(infoI, infoR, kInitiatorGuid, RakNetGUID(kResponderGuid.g ^ 1), &k));
		expectDifferent(k, "responder guid");
	}
	{
		// The local side's own info only enters the transcript, not the DH.
		CryptInfo changed = infoI;
		changed.mode = EncryptionMode::Plaintext;
		SessionKeys k;
		ASSERT_TRUE(derive(changed, infoR, kInitiatorGuid, kResponderGuid, &k));
		expectDifferent(k, "mode");
	}
	{
		CryptInfo changed = infoI;
		changed.publicKey[31] ^= 0x01;
		SessionKeys k;
		ASSERT_TRUE(derive(changed, infoR, kInitiatorGuid, kResponderGuid, &k));
		expectDifferent(k, "transcript public key");
	}
	{
		const CryptInfo other = EphemeralKeyPair().MakeCryptInfo();
		SessionKeys k;
		ASSERT_TRUE(derive(infoI, other, kInitiatorGuid, kResponderGuid, &k));
		expectDifferent(k, "remote public key");
	}
}

// A copy would outlive the wipe DeriveSessionKeys performs on the original.
static_assert(!std::is_copy_constructible<EphemeralKeyPair>::value, "EphemeralKeyPair must not be copyable");
static_assert(!std::is_copy_assignable<EphemeralKeyPair>::value, "EphemeralKeyPair must not be copy-assignable");

TEST(SessionCrypto, KeyPairFromSecretKeyForTestingRebuildsTheSamePair)
{
	EphemeralKeyPair original;
	const CryptInfo info = original.MakeCryptInfo();
	EphemeralKeyPair rebuilt = EphemeralKeyPairTestAccess::FromSecretKey(EphemeralKeyPairTestAccess::SecretKey(original), info.nonce);
	const CryptInfo again = rebuilt.MakeCryptInfo();
	EXPECT_EQ(0, memcmp(info.publicKey, again.publicKey, sizeof(info.publicKey)));
	EXPECT_EQ(info.nonce, again.nonce);
	EXPECT_FALSE(rebuilt.IsWiped());
}

TEST(SessionCrypto, LowOrderPublicKeyIsRejected)
{
	EphemeralKeyPair local;
	const CryptInfo infoLocal = local.MakeCryptInfo();
	CryptInfo remote = EphemeralKeyPair().MakeCryptInfo();
	memset(remote.publicKey, 0, sizeof(remote.publicKey));
	SessionKeys k;
	EXPECT_FALSE(DeriveSessionKeys(local, true, infoLocal, remote, kInitiatorGuid, kResponderGuid, &k));
	EXPECT_TRUE(local.IsWiped());
}

TEST(SessionCrypto, PlaintextRemoteInfoIsRejected)
{
	EphemeralKeyPair local;
	const CryptInfo infoLocal = local.MakeCryptInfo();
	CryptInfo remote = EphemeralKeyPair().MakeCryptInfo();
	remote.mode = EncryptionMode::Plaintext;
	SessionKeys k;
	EXPECT_FALSE(DeriveSessionKeys(local, false, remote, infoLocal, kInitiatorGuid, kResponderGuid, &k));
}

TEST(SessionCrypto, DerivingWipesTheEphemeralKeyPair)
{
	EphemeralKeyPair i;
	EphemeralKeyPair r;
	const CryptInfo infoI = i.MakeCryptInfo();
	const CryptInfo infoR = r.MakeCryptInfo();
	SessionKeys k;
	ASSERT_TRUE(DeriveSessionKeys(i, true, infoI, infoR, kInitiatorGuid, kResponderGuid, &k));
	EXPECT_TRUE(i.IsWiped());
	const uint8_t zero[32] = {};
	EXPECT_EQ(0, memcmp(EphemeralKeyPairTestAccess::SecretKey(i), zero, 32));
	// A wiped key pair cannot derive again.
	EXPECT_FALSE(DeriveSessionKeys(i, true, infoI, infoR, kInitiatorGuid, kResponderGuid, &k));
}

TEST(SessionCrypto, MakeCryptInfoIsStableAndFresh)
{
	EphemeralKeyPair a;
	EphemeralKeyPair b;
	const CryptInfo a1 = a.MakeCryptInfo();
	const CryptInfo a2 = a.MakeCryptInfo();
	const CryptInfo b1 = b.MakeCryptInfo();
	EXPECT_EQ(EncryptionMode::Required, a1.mode);
	EXPECT_EQ(a1.nonce, a2.nonce);
	EXPECT_EQ(0, memcmp(a1.publicKey, a2.publicKey, 32));
	EXPECT_NE(0, memcmp(a1.publicKey, b1.publicKey, 32));
	EXPECT_NE(a1.nonce, b1.nonce);
}

// ------------------------------------------------------------------ crypt info

TEST(SessionCrypto, CryptInfoRoundTripsThroughBitStream)
{
	CryptInfo in = EphemeralKeyPair().MakeCryptInfo();
	in.nonce = 0x0807060504030201ull;
	BitStream bs;
	in.Serialize(&bs);
	ASSERT_EQ((unsigned) CryptInfo::kSerializedBytes, bs.GetNumberOfBytesUsed());
	const uint8_t *raw = bs.GetData();
	EXPECT_EQ(0, raw[0]);
	EXPECT_EQ(0, memcmp(raw + 1, in.publicKey, 32));
	for (int i = 0; i < 8; i++)
		EXPECT_EQ(i + 1, raw[33 + i]) << "nonce byte " << i << " is not little-endian";

	CryptInfo out;
	ASSERT_TRUE(out.Deserialize(&bs));
	EXPECT_EQ(in.mode, out.mode);
	EXPECT_EQ(in.nonce, out.nonce);
	EXPECT_EQ(0, memcmp(in.publicKey, out.publicKey, 32));

	CryptInfo plain;
	plain.mode = EncryptionMode::Plaintext;
	memset(plain.publicKey, 0, 32);
	plain.nonce = 0;
	BitStream bs2;
	plain.Serialize(&bs2);
	CryptInfo plainOut;
	ASSERT_TRUE(plainOut.Deserialize(&bs2));
	EXPECT_EQ(EncryptionMode::Plaintext, plainOut.mode);
}

TEST(SessionCrypto, CryptInfoRejectsUnknownMode)
{
	uint8_t raw[CryptInfo::kSerializedBytes] = {};
	raw[0] = 2;
	BitStream bs(raw, sizeof(raw), true);
	CryptInfo out;
	EXPECT_FALSE(out.Deserialize(&bs));

	raw[0] = 0xFF;
	BitStream bs2(raw, sizeof(raw), true);
	EXPECT_FALSE(out.Deserialize(&bs2));
}

TEST(SessionCrypto, CryptInfoRejectsShortRead)
{
	uint8_t raw[CryptInfo::kSerializedBytes] = {};
	for (int len = 0; len < CryptInfo::kSerializedBytes; len++)
	{
		BitStream bs(raw, (unsigned) len, true);
		CryptInfo out;
		EXPECT_FALSE(out.Deserialize(&bs)) << "length " << len;
	}
}

// ------------------------------------------------------------- datagram cipher

TEST(SessionCrypto, RoundTrips)
{
	CipherPair c;
	for (int len : {0, 1, 1200})
	{
		const std::vector<uint8_t> pt = Payload(len);
		const std::vector<uint8_t> sealed = c.Seal(pt);
		std::vector<uint8_t> opened;
		ASSERT_EQ(len, c.Open(sealed, &opened)) << "length " << len;
		EXPECT_EQ(pt, opened);
	}
}

TEST(SessionCrypto, RoundTripsAtSequenceByteBoundaries)
{
	const uint64_t sequences[] = {0, 255, 256, 65535, 65536, (1ull << 56) - 1, 1ull << 56, UINT64_MAX - 1};
	const std::vector<uint8_t> pt = Payload(100);
	for (uint64_t seq : sequences)
	{
		CipherPair c;
		c.sender.SetNextSendSequenceForTesting(seq);
		const std::vector<uint8_t> sealed = c.Seal(pt);
		const int n = SequenceBytes(seq);
		ASSERT_EQ(pt.size() + 1 + (size_t) n + 16, sealed.size()) << "sequence " << seq;
		EXPECT_EQ((uint8_t) (0x80 | ((n - 1) << 4)), sealed[0]) << "sequence " << seq;
		uint64_t wire = 0;
		for (int i = 0; i < n; i++)
			wire |= (uint64_t) sealed[1 + (size_t) i] << (8 * i);
		EXPECT_EQ(seq, wire);
		EXPECT_EQ(seq + 1, c.sender.NextSendSequence());
		std::vector<uint8_t> opened;
		ASSERT_EQ((int) pt.size(), c.Open(sealed, &opened)) << "sequence " << seq;
		EXPECT_EQ(pt, opened);
	}
}

TEST(SessionCrypto, SealRejectsTooSmallOutput)
{
	CipherPair c;
	const std::vector<uint8_t> pt = Payload(10);
	std::vector<uint8_t> out(pt.size() + 1 + 1 + 16 - 1);
	EXPECT_EQ(-1, c.sender.Seal(pt.data(), (int) pt.size(), out.data(), (int) out.size()));
	// A refused seal must not consume a sequence number.
	EXPECT_EQ(0u, c.sender.NextSendSequence());
	out.resize(out.size() + 1);
	EXPECT_EQ((int) out.size(), c.sender.Seal(pt.data(), (int) pt.size(), out.data(), (int) out.size()));
}

TEST(SessionCrypto, SealAndOpenWithoutKeysFail)
{
	DatagramCipher c;
	EXPECT_FALSE(c.HasKeys());
	const std::vector<uint8_t> pt = Payload(10);
	std::vector<uint8_t> out(64);
	EXPECT_EQ(-1, c.Seal(pt.data(), (int) pt.size(), out.data(), (int) out.size()));
	uint8_t *p = nullptr;
	EXPECT_EQ(-1, c.Open(out.data(), (int) out.size(), &p));
}

TEST(SessionCrypto, ClearForgetsKeysAndCounters)
{
	CipherPair c;
	c.Seal(Payload(4));
	EXPECT_TRUE(c.sender.HasKeys());
	EXPECT_EQ(1u, c.sender.NextSendSequence());
	c.sender.Clear();
	EXPECT_FALSE(c.sender.HasKeys());
	EXPECT_EQ(0u, c.sender.NextSendSequence());
}

TEST(SessionCrypto, TamperingAnyByteIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> sealed = c.Seal(Payload(40));
	for (size_t i = 0; i < sealed.size(); i++)
	{
		for (uint8_t mask : {(uint8_t) 0x01, (uint8_t) 0x10, (uint8_t) 0x80})
		{
			std::vector<uint8_t> bad = sealed;
			bad[i] ^= mask;
			EXPECT_EQ(-1, c.Open(bad)) << "byte " << i << " mask " << (int) mask;
		}
	}
	// None of the rejections advanced the window.
	EXPECT_EQ(40, c.Open(sealed));
}

TEST(SessionCrypto, WrongKeyIsRejected)
{
	CipherPair a;
	CipherPair b;
	const std::vector<uint8_t> sealed = a.Seal(Payload(40));
	EXPECT_EQ(-1, b.Open(sealed));
	// The sender's own receive key is the other direction's key.
	DatagramCipher self;
	self.SetKeys(a.keys.initiator);
	std::vector<uint8_t> copy = sealed;
	uint8_t *p = nullptr;
	EXPECT_EQ(-1, self.Open(copy.data(), (int) copy.size(), &p));
}

TEST(SessionCrypto, WrongAadIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> pt = Payload(20);
	// Same key, nonce and layout, but sealed under another protocol version.
	const int n = 1;
	const uint8_t prefix = 0x80;
	std::vector<uint8_t> out(1 + n + pt.size() + 16);
	out[0] = prefix;
	out[1] = 0;
	uint8_t nonce[12];
	detail::BuildNonce(0, nonce);
	const uint8_t aad[2] = {(uint8_t) (RAKNET_PROTOCOL_VERSION + 1), prefix};
	ASSERT_TRUE(detail::AeadSeal(c.keys.initiator.send, nonce, aad, sizeof(aad), pt.data(), pt.size(), out.data() + 1 + n));
	EXPECT_EQ(-1, c.Open(out));
	// The same datagram under the right AAD is accepted.
	EXPECT_EQ((int) pt.size(), c.Open(HandSeal(c.keys.initiator.send, 0, pt)));
}

TEST(SessionCrypto, ReplayIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> sealed = c.Seal(Payload(40));
	EXPECT_EQ(40, c.Open(sealed));
	EXPECT_EQ(-1, c.Open(sealed));
}

TEST(SessionCrypto, TooOldIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> pt = Payload(8);
	c.sender.SetNextSendSequenceForTesting(44);
	const std::vector<uint8_t> s44 = c.Seal(pt);
	const std::vector<uint8_t> s45 = c.Seal(pt);
	c.sender.SetNextSendSequenceForTesting(300);
	const std::vector<uint8_t> s300 = c.Seal(pt);

	ASSERT_EQ(8, c.Open(s300));
	EXPECT_EQ(-1, c.Open(s44));
	EXPECT_EQ(8, c.Open(s45));
}

TEST(SessionCrypto, ReorderedWithinWindowIsAccepted)
{
	CipherPair c;
	std::vector<std::vector<uint8_t>> sealed;
	for (int i = 0; i < 200; i++)
		sealed.push_back(c.Seal(Payload(i % 17)));
	// Deliver in a scrambled but complete order.
	for (int k = 0; k < 200; k++)
	{
		const size_t i = (size_t) ((k * 73) % 200);
		EXPECT_EQ((int) (i % 17), c.Open(sealed[i])) << "sequence " << i;
	}
	for (const auto &s : sealed)
		EXPECT_EQ(-1, c.Open(s));
}

TEST(SessionCrypto, FailedDecryptDoesNotAdvanceWindow)
{
	CipherPair c;
	const std::vector<uint8_t> pt = Payload(8);
	c.sender.SetNextSendSequenceForTesting(10);
	const std::vector<uint8_t> s10 = c.Seal(pt);
	c.sender.SetNextSendSequenceForTesting(1000);
	const std::vector<uint8_t> s1000 = c.Seal(pt);

	std::vector<uint8_t> forged = s1000;
	forged.back() ^= 0x01;
	EXPECT_EQ(-1, c.Open(forged));
	EXPECT_EQ(8, c.Open(s10));
	EXPECT_EQ(8, c.Open(s1000));
}

TEST(SessionCrypto, ReservedSequenceIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> pt = Payload(8);

	// The sender refuses to emit the reserved value.
	c.sender.SetNextSendSequenceForTesting(UINT64_MAX);
	std::vector<uint8_t> out(pt.size() + DatagramCipher::kMaxOverhead);
	EXPECT_EQ(-1, c.sender.Seal(pt.data(), (int) pt.size(), out.data(), (int) out.size()));
	EXPECT_EQ(UINT64_MAX, c.sender.NextSendSequence());

	// A correctly sealed datagram carrying it is still rejected.
	EXPECT_EQ(-1, c.Open(HandSeal(c.keys.initiator.send, UINT64_MAX, pt)));
	// The hand-built layout itself is valid: the largest legal sequence opens.
	EXPECT_EQ(8, c.Open(HandSeal(c.keys.initiator.send, UINT64_MAX - 1, pt)));
}

TEST(SessionCrypto, MalformedPrefixIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> sealed = c.Seal(Payload(8));
	for (uint8_t prefix : {(uint8_t) 0x00, (uint8_t) 0x81, (uint8_t) 0x7F})
	{
		std::vector<uint8_t> bad = sealed;
		bad[0] = prefix;
		EXPECT_EQ(-1, c.Open(bad)) << "prefix " << (int) prefix;
	}
}

TEST(SessionCrypto, ShortDatagramIsRejected)
{
	CipherPair c;
	const std::vector<uint8_t> sealed = c.Seal(Payload(0));
	ASSERT_EQ(18u, sealed.size());
	for (size_t len = 0; len < 18; len++)
	{
		std::vector<uint8_t> shortened(sealed.begin(), sealed.begin() + (std::ptrdiff_t) len);
		uint8_t *p = nullptr;
		EXPECT_EQ(-1, c.receiver.Open(shortened.empty() ? nullptr : shortened.data(), (int) len, &p)) << "length " << len;
	}
	// An 8-byte sequence needs 1 + 8 + 16 bytes; anything shorter is rejected.
	c.sender.SetNextSendSequenceForTesting(1ull << 56);
	const std::vector<uint8_t> wide = c.Seal(Payload(0));
	ASSERT_EQ(25u, wide.size());
	for (size_t len = 0; len < 25; len++)
	{
		std::vector<uint8_t> shortened(wide.begin(), wide.begin() + (std::ptrdiff_t) len);
		EXPECT_EQ(-1, c.Open(shortened)) << "length " << len;
	}
	EXPECT_EQ(0, c.Open(sealed));
	EXPECT_EQ(0, c.Open(wide));
}

// ---------------------------------------------------------------- test vectors

TEST(SessionCrypto, HkdfMatchesRfc5869TestCase1)
{
	const std::vector<uint8_t> ikm = FromHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
	const std::vector<uint8_t> salt = FromHex("000102030405060708090a0b0c");
	const std::vector<uint8_t> info = FromHex("f0f1f2f3f4f5f6f7f8f9");
	const std::vector<uint8_t> expectedPrk = FromHex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
	const std::vector<uint8_t> expectedOkm = FromHex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

	uint8_t prk[32];
	detail::HkdfExtract(salt.data(), salt.size(), ikm.data(), ikm.size(), prk);
	EXPECT_EQ(expectedPrk, std::vector<uint8_t>(prk, prk + 32));

	std::vector<uint8_t> okm(42);
	ASSERT_TRUE(detail::HkdfExpand(prk, info.data(), info.size(), okm.data(), okm.size()));
	EXPECT_EQ(expectedOkm, okm);

	// RFC 5869 caps the output at 255 blocks.
	std::vector<uint8_t> tooLong(255 * 32 + 1);
	EXPECT_FALSE(detail::HkdfExpand(prk, info.data(), info.size(), tooLong.data(), tooLong.size()));
}

TEST(SessionCrypto, AeadMatchesRfc8439Section2_8_2)
{
	const char *text = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
	const std::vector<uint8_t> pt(text, text + strlen(text));
	const std::vector<uint8_t> key = FromHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
	const std::vector<uint8_t> nonce = FromHex("070000004041424344454647");
	const std::vector<uint8_t> aad = FromHex("50515253c0c1c2c3c4c5c6c7");
	const std::vector<uint8_t> expectedTag = FromHex("1ae10b594f09e26a7e902ecbd0600691");
	ASSERT_EQ(114u, pt.size());

	std::vector<uint8_t> out(pt.size() + 16);
	ASSERT_TRUE(detail::AeadSeal(key.data(), nonce.data(), aad.data(), aad.size(), pt.data(), pt.size(), out.data()));
	EXPECT_EQ(expectedTag, std::vector<uint8_t>(out.end() - 16, out.end()));

	std::vector<uint8_t> back(pt.size());
	ASSERT_TRUE(detail::AeadOpen(key.data(), nonce.data(), aad.data(), aad.size(), out.data(), out.size(), back.data()));
	EXPECT_EQ(pt, back);

	uint8_t built[12];
	detail::BuildNonce(0x4746454443424140ull, built);
	EXPECT_EQ(FromHex("000000004041424344454647"), std::vector<uint8_t>(built, built + 12));
}

// ---------------------------------------------------------------- replay window

TEST(SessionCrypto, ReplayWindowHandlesSequencesNearTheTop)
{
	ReplayWindow w;
	EXPECT_TRUE(w.AlreadyReceived(UINT64_MAX));
	EXPECT_FALSE(w.AlreadyReceived(UINT64_MAX - 1));
	w.Advance(UINT64_MAX - 1);
	EXPECT_TRUE(w.AlreadyReceived(UINT64_MAX - 1));
	// No overflow: values just below the top are inside the window, not "too old".
	EXPECT_FALSE(w.AlreadyReceived(UINT64_MAX - 2));
	EXPECT_FALSE(w.AlreadyReceived(UINT64_MAX - 1 - (ReplayWindow::kSize - 1)));
	EXPECT_TRUE(w.AlreadyReceived(UINT64_MAX - 1 - ReplayWindow::kSize));
	EXPECT_TRUE(w.AlreadyReceived(0));
	// Advancing with the reserved value is ignored.
	w.Advance(UINT64_MAX);
	EXPECT_FALSE(w.AlreadyReceived(UINT64_MAX - 2));
}

TEST(SessionCrypto, ReplayWindowStartsEmpty)
{
	ReplayWindow w;
	for (uint64_t s = 0; s < 600; s++)
		EXPECT_FALSE(w.AlreadyReceived(s)) << s;
	w.Advance(0);
	EXPECT_TRUE(w.AlreadyReceived(0));
	EXPECT_FALSE(w.AlreadyReceived(256));
}
