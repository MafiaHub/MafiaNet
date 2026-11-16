/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Pinned server identity: a server's long-term Ed25519 key signs the handshake
 *  transcript in ID_OPEN_CONNECTION_REPLY_2, and a client that pinned that key
 *  (Connect's PublicKey*) accepts nothing else.
 *
 *  ServerIdentityCrypto.* covers the session_crypto helpers (key pair, identity
 *  block, signed transcript, key schedule) against known answers computed
 *  independently with OpenSSL. ServerIdentity.* runs whole peers on the
 *  SimulatedNetwork (see Tests/Support/SimulatedNetwork.h).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sodium.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/session_crypto.h"
#include "mafianet/statistics.h"
#include "mafianet/version.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_TEST_PAYLOAD = ID_USER_PACKET_ENUM;

std::vector<uint8_t> FromHex(const char *hex)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2)
	{
		unsigned int byte = 0;
		std::sscanf(hex + i, "%2x", &byte);
		out.push_back((uint8_t) byte);
	}
	return out;
}

std::vector<uint8_t> Bytes(const uint8_t *data, size_t n)
{
	return std::vector<uint8_t>(data, data + n);
}

// An identity key pair from a fixed 32-byte seed, so known answers can be checked.
struct IdentityKey
{
	uint8_t publicKey[kIdentityPublicKeyBytes];
	uint8_t secretKey[kIdentitySecretKeyBytes];

	static IdentityKey FromSeed(uint8_t first)
	{
		uint8_t seed[32];
		for (int i = 0; i < 32; ++i)
			seed[i] = (uint8_t) (first + i);
		IdentityKey key;
		EXPECT_EQ(0, sodium_init() < 0);
		crypto_sign_ed25519_seed_keypair(key.publicKey, key.secretKey, seed);
		return key;
	}
	static IdentityKey Generate()
	{
		IdentityKey key;
		GenerateIdentityKeyPair(key.publicKey, key.secretKey);
		return key;
	}
	PublicKey Pin() const
	{
		PublicKey pin;
		std::memcpy(pin.serverIdentity, publicKey, sizeof(pin.serverIdentity));
		return pin;
	}
};

CryptInfo MakeInfo(uint8_t keyFirst, uint64_t nonce)
{
	CryptInfo info;
	info.mode = EncryptionMode::Required;
	for (int i = 0; i < 32; ++i)
		info.publicKey[i] = (uint8_t) (keyFirst + i);
	info.nonce = nonce;
	return info;
}

// ---------------------------------------------------------------- simulated-network helpers

size_t CountOf(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	size_t n = 0;
	for (const auto &message : net.Inbox(peer))
		n += message.id == id;
	return n;
}

ConnectionAttemptResult ConnectTo(SimulatedNetwork &net, RakPeer *from, const RakPeer *to, PublicKey *pin)
{
	const SystemAddress target = net.AddressOf(to);
	char host[64];
	target.ToString(false, host, sizeof(host));
	return from->Connect(host, target.GetPort(), nullptr, 0, pin);
}

bool Connected(SimulatedNetwork &net, const RakPeer *client, const RakPeer *server)
{
	return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED) && net.Received(server, ID_NEW_INCOMING_CONNECTION);
}

void SendNumber(RakPeer *from, const SystemAddress &to, uint32_t number)
{
	BitStream bs;
	bs.Write(ID_TEST_PAYLOAD);
	bs.Write(number);
	ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, to, false));
}

// Data flows both ways under the session keys.
void ExpectDataFlows(SimulatedNetwork &net, RakPeer *a, RakPeer *b)
{
	SendNumber(a, net.AddressOf(b), 1);
	SendNumber(b, net.AddressOf(a), 2);
	EXPECT_TRUE(net.RunUntil([&]
	                         { return net.Received(a, ID_TEST_PAYLOAD) && net.Received(b, ID_TEST_PAYLOAD); }, 2000));
}

// The client side of a failed attempt holds nothing: no connection, no pending request.
void ExpectClientHoldsNothing(SimulatedNetwork &net, RakPeer *client, const RakPeer *server)
{
	EXPECT_EQ(0u, client->NumberOfConnections());
	EXPECT_EQ(IS_NOT_CONNECTED, client->GetConnectionState(net.AddressOf(server)));
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
	uint8_t key[32];
	EXPECT_FALSE(client->GetRemoteIdentityKey(net.AddressOf(server), key));
}

// The fields of a captured ID_OPEN_CONNECTION_REPLY_2 and where they sit.
struct Reply2
{
	std::vector<unsigned char> bytes;
	CryptInfo serverInfo;
	IdentityBlock identity;
	size_t serverInfoOffset = 0;
	size_t identityOffset = 0;

	bool Parse()
	{
		BitStream bs(bytes.data(), (unsigned int) bytes.size(), false);
		bs.IgnoreBytes(1 + 16);
		uint64_t nonceEcho;
		RakNetGUID guid;
		SystemAddress address;
		uint16_t mtu;
		if (!bs.Read(nonceEcho) || !bs.Read(guid) || !bs.Read(address) || !bs.Read(mtu))
			return false;
		serverInfoOffset = BITS_TO_BYTES(bs.GetReadOffset());
		if (!serverInfo.Deserialize(&bs))
			return false;
		identityOffset = BITS_TO_BYTES(bs.GetReadOffset());
		return identity.Deserialize(&bs) && BITS_TO_BYTES(bs.GetReadOffset()) == bytes.size();
	}
	// The same reply with a different identity block.
	std::vector<unsigned char> WithIdentity(const IdentityBlock &block) const
	{
		std::vector<unsigned char> out(bytes.begin(), bytes.begin() + (long) identityOffset);
		uint8_t raw[IdentityBlock::kMaxSerializedBytes];
		const int n = block.SerializeTo(raw);
		out.insert(out.end(), raw, raw + n);
		return out;
	}
};

// Runs a connect up to the server's REPLY_2 while holding back everything the server sends; REPLY_1 is handed
// to the client directly. The test then decides which REPLY_2 the client gets (InjectReply2), and Release()
// opens the server-to-client link again.
struct HeldHandshake
{
	SimulatedNetwork &net;
	RakPeer *client;
	RakPeer *server;
	std::vector<std::vector<unsigned char>> fromServer;
	std::vector<unsigned char> request2;
	Reply2 reply2;

	HeldHandshake(SimulatedNetwork &n, RakPeer *c, RakPeer *s)
	    : net(n), client(c), server(s)
	{
		const SystemAddress serverAddress = net.AddressOf(server);
		const SystemAddress clientAddress = net.AddressOf(client);
		net.SetTap([this, serverAddress, clientAddress](const SystemAddress &from, const SystemAddress &to, const char *data, int length)
		           {
			           if (length <= 0)
				           return;
			           std::vector<unsigned char> d(data, data + length);
			           if (from == serverAddress && to == clientAddress)
				           fromServer.push_back(d);
			           if (from == clientAddress && d[0] == ID_OPEN_CONNECTION_REQUEST_2)
				           request2 = d; });
		SimulatedNetwork::LinkRule dark;
		dark.dark = true;
		net.SetLink(server, client, dark);
	}

	const std::vector<unsigned char> *LastFromServer(MessageID id) const
	{
		for (auto it = fromServer.rbegin(); it != fromServer.rend(); ++it)
			if ((*it)[0] == id)
				return &*it;
		return nullptr;
	}

	// Connect (with \a pin, or none) and run until the server has sent its REPLY_2.
	bool RunToReply2(PublicKey *pin)
	{
		if (ConnectTo(net, client, server, pin) != CONNECTION_ATTEMPT_STARTED)
			return false;
		if (!net.RunUntil([&]
		                  { return LastFromServer(ID_OPEN_CONNECTION_REPLY_1) != nullptr; }, 2000, 1))
			return false;
		const std::vector<unsigned char> reply1 = *LastFromServer(ID_OPEN_CONNECTION_REPLY_1);
		net.InjectDatagram(net.AddressOf(server), client, reply1.data(), (int) reply1.size());
		if (!net.RunUntil([&]
		                  { return LastFromServer(ID_OPEN_CONNECTION_REPLY_2) != nullptr; }, 2000, 1))
			return false;
		reply2.bytes = *LastFromServer(ID_OPEN_CONNECTION_REPLY_2);
		return reply2.Parse() && !request2.empty();
	}

	// The client crypt info from the captured REQUEST_2: id (1) | offline id (16) | challenge (8) | crypt info (41).
	CryptInfo ClientInfo() const
	{
		BitStream bs(const_cast<unsigned char *>(request2.data()), (unsigned int) request2.size(), false);
		bs.IgnoreBytes(1 + 16 + 8);
		CryptInfo info;
		EXPECT_TRUE(info.Deserialize(&bs));
		return info;
	}

	void InjectReply2(const std::vector<unsigned char> &bytes)
	{
		net.InjectDatagram(net.AddressOf(server), client, bytes.data(), (int) bytes.size());
		net.Step(10);
	}

	void Release() { net.SetLink(server, client, SimulatedNetwork::LinkRule()); }
};
} // namespace

// ============================================================ session_crypto helpers

TEST(ServerIdentityCrypto, GeneratedKeyPairRoundTrips)
{
	IdentityKey key = IdentityKey::Generate();
	uint8_t derived[32];
	ASSERT_TRUE(IdentityPublicKeyFromSecret(key.secretKey, derived));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(derived, 32));

	const CryptInfo client = MakeInfo(0x40, 1);
	const CryptInfo server = MakeInfo(0x60, 2);
	IdentityBlock block;
	ASSERT_TRUE(SignServerIdentity(key.secretKey, client, server, &block));
	EXPECT_TRUE(block.present);
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(block.publicKey, 32));
	EXPECT_TRUE(VerifyServerIdentity(block, client, server));

	IdentityKey other = IdentityKey::Generate();
	EXPECT_NE(Bytes(key.publicKey, 32), Bytes(other.publicKey, 32));
}

TEST(ServerIdentityCrypto, MalformedSecretKeyIsRejected)
{
	IdentityKey key = IdentityKey::Generate();
	for (int index : {0, 31, 32, 63})
	{
		SCOPED_TRACE(index);
		uint8_t bad[64];
		std::memcpy(bad, key.secretKey, sizeof(bad));
		bad[index] ^= 0x01;
		uint8_t out[32];
		std::memset(out, 0xAA, sizeof(out));
		EXPECT_FALSE(IdentityPublicKeyFromSecret(bad, out));
		EXPECT_EQ(std::vector<uint8_t>(32, 0), Bytes(out, 32));
		IdentityBlock block;
		EXPECT_FALSE(SignServerIdentity(bad, MakeInfo(1, 1), MakeInfo(2, 2), &block));
		EXPECT_FALSE(block.present);
	}
}

// The signed bytes, as documented in encryption-wire-format.rst.
TEST(ServerIdentityCrypto, TranscriptLayout)
{
	CryptInfo client = MakeInfo(0x40, 0x0102030405060708ull);
	CryptInfo server = MakeInfo(0x60, 0x1112131415161718ull);
	server.mode = EncryptionMode::Plaintext;
	uint8_t transcript[kIdentityTranscriptBytes];
	BuildIdentityTranscript(client, server, transcript);
	ASSERT_EQ(107, kIdentityTranscriptBytes);

	std::vector<uint8_t> expected;
	const char label[] = "MafiaNet server identity";
	expected.insert(expected.end(), label, label + 24);
	expected.push_back((uint8_t) RAKNET_PROTOCOL_VERSION);
	expected.push_back(0); // client mode
	for (int i = 0; i < 32; ++i)
		expected.push_back((uint8_t) (0x40 + i));
	for (uint8_t b : {(uint8_t) 8, (uint8_t) 7, (uint8_t) 6, (uint8_t) 5, (uint8_t) 4, (uint8_t) 3, (uint8_t) 2, (uint8_t) 1})
		expected.push_back(b);
	expected.push_back(1); // server mode Plaintext
	for (int i = 0; i < 32; ++i)
		expected.push_back((uint8_t) (0x60 + i));
	for (uint8_t b : {(uint8_t) 0x18, (uint8_t) 0x17, (uint8_t) 0x16, (uint8_t) 0x15, (uint8_t) 0x14, (uint8_t) 0x13, (uint8_t) 0x12, (uint8_t) 0x11})
		expected.push_back(b);
	EXPECT_EQ(expected, Bytes(transcript, sizeof(transcript)));
}

// Known answer: the public key and signature computed with OpenSSL (Ed25519 is deterministic) from the seed
// 00..1f over the documented transcript.
TEST(ServerIdentityCrypto, SignatureMatchesKnownAnswer)
{
	const IdentityKey key = IdentityKey::FromSeed(0x00);
	EXPECT_EQ(FromHex("03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"), Bytes(key.publicKey, 32));

	IdentityBlock block;
	ASSERT_TRUE(SignServerIdentity(key.secretKey, MakeInfo(0x40, 0x0102030405060708ull), MakeInfo(0x60, 0x1112131415161718ull), &block));
	EXPECT_EQ(FromHex("320b75b3eead098052fdf46891bf766bcb5cf9632e30983985786a56ea6b818f"
	                  "1a23cb51c7e8a3ff84e2728a62cf0bb55f290e86946bee04e772d46452b74f0f"),
	          Bytes(block.signature, 64));
}

TEST(ServerIdentityCrypto, VerifyRejectsAnyChange)
{
	const IdentityKey key = IdentityKey::FromSeed(0x10);
	const CryptInfo client = MakeInfo(0x40, 7);
	const CryptInfo server = MakeInfo(0x60, 9);
	IdentityBlock block;
	ASSERT_TRUE(SignServerIdentity(key.secretKey, client, server, &block));
	ASSERT_TRUE(VerifyServerIdentity(block, client, server));

	for (int i = 0; i < kIdentitySignatureBytes; ++i)
	{
		IdentityBlock changed = block;
		changed.signature[i] ^= 0x01;
		EXPECT_FALSE(VerifyServerIdentity(changed, client, server)) << "signature byte " << i;
	}
	for (int i = 0; i < kIdentityPublicKeyBytes; ++i)
	{
		IdentityBlock changed = block;
		changed.publicKey[i] ^= 0x01;
		EXPECT_FALSE(VerifyServerIdentity(changed, client, server)) << "public key byte " << i;
	}
	IdentityBlock absent = block;
	absent.present = false;
	EXPECT_FALSE(VerifyServerIdentity(absent, client, server));

	// Every transcript field: both modes, both public keys, both nonces.
	for (int which = 0; which < 2; ++which)
	{
		for (int field = 0; field < 3; ++field)
		{
			CryptInfo c = client;
			CryptInfo s = server;
			CryptInfo &target = which == 0 ? c : s;
			if (field == 0)
				target.mode = EncryptionMode::Plaintext;
			else if (field == 1)
				target.publicKey[5] ^= 0x01;
			else
				target.nonce ^= 1;
			EXPECT_FALSE(VerifyServerIdentity(block, c, s)) << (which == 0 ? "client" : "server") << " field " << field;
		}
	}
	// Swapping the roles is a different transcript.
	EXPECT_FALSE(VerifyServerIdentity(block, server, client));
}

TEST(ServerIdentityCrypto, IdentityBlockSerialization)
{
	IdentityBlock absent;
	uint8_t raw[IdentityBlock::kMaxSerializedBytes];
	EXPECT_EQ(1, absent.SerializeTo(raw));
	EXPECT_EQ(0, raw[0]);
	EXPECT_EQ(1, absent.SerializedBytes());

	const IdentityKey key = IdentityKey::FromSeed(0x20);
	IdentityBlock present;
	ASSERT_TRUE(SignServerIdentity(key.secretKey, MakeInfo(1, 1), MakeInfo(2, 2), &present));
	ASSERT_EQ(97, present.SerializeTo(raw));
	EXPECT_EQ(97, present.SerializedBytes());
	EXPECT_EQ(1, raw[0]);
	EXPECT_EQ(Bytes(present.publicKey, 32), Bytes(raw + 1, 32));
	EXPECT_EQ(Bytes(present.signature, 64), Bytes(raw + 33, 64));

	BitStream out;
	present.Serialize(&out);
	absent.Serialize(&out);
	ASSERT_EQ(98u, out.GetNumberOfBytesUsed());
	IdentityBlock a, b;
	ASSERT_TRUE(a.Deserialize(&out));
	ASSERT_TRUE(b.Deserialize(&out));
	EXPECT_TRUE(a.present);
	EXPECT_EQ(Bytes(present.signature, 64), Bytes(a.signature, 64));
	EXPECT_FALSE(b.present);
}

TEST(ServerIdentityCrypto, MalformedIdentityBlockIsRejected)
{
	for (uint8_t flag : {(uint8_t) 2, (uint8_t) 0x80, (uint8_t) 0xFF})
	{
		uint8_t raw[97] = {};
		raw[0] = flag;
		BitStream bs(raw, sizeof(raw), false);
		IdentityBlock block;
		EXPECT_FALSE(block.Deserialize(&bs)) << "flag " << (int) flag;
	}
	for (unsigned int length : {0u, 1u, 33u, 96u})
	{
		uint8_t raw[97] = {};
		raw[0] = 1;
		BitStream bs(raw, length, false);
		IdentityBlock block;
		EXPECT_FALSE(block.Deserialize(&bs)) << "length " << length;
	}
}

// Known answer for the key schedule with and without an identity block: X25519, HKDF-SHA256 and the HKDF info
// layout of encryption-wire-format.rst, computed independently with OpenSSL and Python's hmac.
TEST(ServerIdentityCrypto, KeyScheduleIncludesTheIdentityBlock)
{
	uint8_t initiatorSecret[32], responderSecret[32];
	for (int i = 0; i < 32; ++i)
	{
		initiatorSecret[i] = (uint8_t) (0x20 + i);
		responderSecret[i] = (uint8_t) (0x80 + i);
	}
	const RakNetGUID initiatorGuid(0x0123456789ABCDEFull);
	const RakNetGUID responderGuid(0xFEDCBA9876543210ull);
	auto infoOf = [](const uint8_t secret[32], uint64_t nonce)
	{
		EphemeralKeyPair pair = EphemeralKeyPairTestAccess::FromSecretKey(secret, nonce);
		return pair.MakeCryptInfo();
	};
	const CryptInfo initiatorInfo = infoOf(initiatorSecret, 0x0102030405060708ull);
	const CryptInfo responderInfo = infoOf(responderSecret, 0x1112131415161718ull);
	EXPECT_EQ(FromHex("358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254"), Bytes(initiatorInfo.publicKey, 32));
	EXPECT_EQ(FromHex("493e82fc74464a59268817623d2053c5eb8e2cc4a988b4fee179ec6b010d531d"), Bytes(responderInfo.publicKey, 32));

	const IdentityKey key = IdentityKey::FromSeed(0x00);
	IdentityBlock identity;
	ASSERT_TRUE(SignServerIdentity(key.secretKey, initiatorInfo, responderInfo, &identity));
	EXPECT_EQ(FromHex("d4753e04deba154c7b7a0fe45939c164cced8bbfe23029053405db8f4b29ba2b"
	                  "dcad2da5eff55fbcde7b0e2e8e3ecbe5085e8ed6e9c1eafd5141e2523208c901"),
	          Bytes(identity.signature, 64));

	auto derive = [&](const IdentityBlock &block, bool initiator, SessionKeys *out)
	{
		EphemeralKeyPair pair = EphemeralKeyPairTestAccess::FromSecretKey(initiator ? initiatorSecret : responderSecret,
		                                                                  initiator ? 0x0102030405060708ull : 0x1112131415161718ull);
		return DeriveSessionKeys(pair, initiator, initiatorInfo, responderInfo, initiatorGuid, responderGuid, block, out);
	};

	SessionKeys withIdentity, responderWithIdentity, without;
	ASSERT_TRUE(derive(identity, true, &withIdentity));
	ASSERT_TRUE(derive(identity, false, &responderWithIdentity));
	ASSERT_TRUE(derive(IdentityBlock(), true, &without));
	// info is 214 bytes with the block and 118 without; keyIR then keyRI.
	EXPECT_EQ(FromHex("1eb1936afdc337087663847a641114873844e581448b7362f812dfb09aab1254"), Bytes(withIdentity.send, 32));
	EXPECT_EQ(FromHex("e06141939b74b15727cf9e210d00e1b8c38b6cf89f3d8135378f6a6b64d45a6e"), Bytes(withIdentity.recv, 32));
	EXPECT_EQ(Bytes(withIdentity.send, 32), Bytes(responderWithIdentity.recv, 32));
	EXPECT_EQ(Bytes(withIdentity.recv, 32), Bytes(responderWithIdentity.send, 32));
	EXPECT_EQ(FromHex("58860a7ed16ddeae757550828bfca82b6eae1e4e696b21fd3616586e52692f7e"), Bytes(without.send, 32));
	EXPECT_EQ(FromHex("f020c90f7e0e1cb497eaf50b6f76bab311134c5874e316fafa95809f9be69ba3"), Bytes(without.recv, 32));

	// The overload without an identity block is the absent block.
	SessionKeys overload;
	EphemeralKeyPair pair = EphemeralKeyPairTestAccess::FromSecretKey(initiatorSecret, 0x0102030405060708ull);
	ASSERT_TRUE(DeriveSessionKeys(pair, true, initiatorInfo, responderInfo, initiatorGuid, responderGuid, &overload));
	EXPECT_EQ(Bytes(without.send, 32), Bytes(overload.send, 32));

	// Any change to the block changes the keys.
	IdentityBlock changed = identity;
	changed.signature[63] ^= 0x01;
	SessionKeys withChanged;
	ASSERT_TRUE(derive(changed, true, &withChanged));
	EXPECT_NE(Bytes(withIdentity.send, 32), Bytes(withChanged.send, 32));
}

// ============================================================ peers on the simulated network

TEST(ServerIdentity, SetIdentityKeyRules)
{
	SimulatedNetwork net;
	RakPeer *started = net.AddPeer(1);
	const IdentityKey key = IdentityKey::Generate();
	uint8_t out[32];
	EXPECT_FALSE(started->GetIdentityPublicKey(out));
	EXPECT_EQ(std::vector<uint8_t>(32, 0), Bytes(out, 32));
	EXPECT_FALSE(started->SetIdentityKey(key.secretKey)) << "must be refused once started";
	EXPECT_FALSE(started->GetIdentityPublicKey(out));

	RakPeer offline;
	uint8_t bad[64];
	std::memcpy(bad, key.secretKey, sizeof(bad));
	bad[40] ^= 0x01;
	EXPECT_FALSE(offline.SetIdentityKey(bad));
	EXPECT_FALSE(offline.GetIdentityPublicKey(out));
	ASSERT_TRUE(offline.SetIdentityKey(key.secretKey));
	ASSERT_TRUE(offline.GetIdentityPublicKey(out));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(out, 32));
	// A malformed replacement changes nothing.
	EXPECT_FALSE(offline.SetIdentityKey(bad));
	ASSERT_TRUE(offline.GetIdentityPublicKey(out));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(out, 32));
	// Replacing, then removing.
	const IdentityKey other = IdentityKey::Generate();
	ASSERT_TRUE(offline.SetIdentityKey(other.secretKey));
	ASSERT_TRUE(offline.GetIdentityPublicKey(out));
	EXPECT_EQ(Bytes(other.publicKey, 32), Bytes(out, 32));
	EXPECT_TRUE(offline.SetIdentityKey(nullptr));
	EXPECT_FALSE(offline.GetIdentityPublicKey(out));
}

// The key survives Shutdown(): a restarted server still signs, and a pinned client still connects.
TEST(ServerIdentity, RestartStillSigns)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(2, 2);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);

	server->Shutdown(0);
	uint8_t out[32];
	ASSERT_TRUE(server->GetIdentityPublicKey(out)) << "Shutdown() must not drop the identity key";
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(out, 32));
	char host[64];
	serverAddress.ToString(false, host, sizeof(host));
	SocketDescriptor descriptor(serverAddress.GetPort(), host);
	ASSERT_EQ(RAKNET_STARTED, server->Startup(2, &descriptor, 1));
	server->SetMaximumIncomingConnections(2);

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, client, server, &pin));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Connected(net, client, server); }, 5000));
	EXPECT_EQ(0u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	uint8_t remote[32];
	ASSERT_TRUE(client->GetRemoteIdentityKey(serverAddress, remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
}

// SetIdentityKey(nullptr) while offline removes the key: the next Startup() answers unsigned, and a pinned
// client gets ID_PUBLIC_KEY_MISMATCH.
TEST(ServerIdentity, SetIdentityKeyNullRemovesTheKey)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(2, 2);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);

	EXPECT_FALSE(server->SetIdentityKey(nullptr)) << "refused while started";
	uint8_t out[32];
	ASSERT_TRUE(server->GetIdentityPublicKey(out));

	server->Shutdown(0);
	EXPECT_TRUE(server->SetIdentityKey(nullptr));
	EXPECT_FALSE(server->GetIdentityPublicKey(out));
	char host[64];
	serverAddress.ToString(false, host, sizeof(host));
	SocketDescriptor descriptor(serverAddress.GetPort(), host);
	ASSERT_EQ(RAKNET_STARTED, server->Startup(2, &descriptor, 1));
	server->SetMaximumIncomingConnections(2);

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, client, server, &pin));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_PUBLIC_KEY_MISMATCH, 5000));
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
}

TEST(ServerIdentity, StaticGenerateIdentityKeyPairForwards)
{
	uint8_t publicKey[32], secretKey[64], derived[32];
	RakPeerInterface::GenerateIdentityKeyPair(publicKey, secretKey);
	ASSERT_TRUE(IdentityPublicKeyFromSecret(secretKey, derived));
	EXPECT_EQ(Bytes(publicKey, 32), Bytes(derived, 32));
	RakPeer offline;
	EXPECT_TRUE(offline.SetIdentityKey(secretKey));
}

TEST(ServerIdentity, PinnedClientConnectsToTheRightServer)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, client, server, &pin));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Connected(net, client, server); }, 5000));
	EXPECT_EQ(0u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));

	uint8_t remote[32];
	ASSERT_TRUE(client->GetRemoteIdentityKey(net.AddressOf(server), remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
	ASSERT_TRUE(client->GetRemoteIdentityKey(server->GetMyGUID(), remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
	// Servers do not authenticate clients.
	EXPECT_FALSE(server->GetRemoteIdentityKey(net.AddressOf(client), remote));
	ExpectDataFlows(net, client, server);
}

TEST(ServerIdentity, PinnedClientRefusesAServerWithADifferentKey)
{
	SimulatedNetwork net;
	const IdentityKey serverKey = IdentityKey::Generate();
	const IdentityKey pinnedKey = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(serverKey.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	PublicKey pin = pinnedKey.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, client, server, &pin));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_PUBLIC_KEY_MISMATCH, 5000));
	const SimulatedNetwork::Message *mismatch = net.Find(client, ID_PUBLIC_KEY_MISMATCH);
	EXPECT_EQ(net.AddressOf(server), mismatch->systemAddress);
	EXPECT_EQ(server->GetMyGUID(), mismatch->guid);
	net.Step(15000); // the whole retry budget: the attempt is over, not retried

	EXPECT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	EXPECT_EQ(0u, CountOf(net, client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(1u, net.Inbox(client).size()) << "unexpected extra message";
	ExpectClientHoldsNothing(net, client, server);
	EXPECT_FALSE(net.Received(server, ID_NEW_INCOMING_CONNECTION));
}

TEST(ServerIdentity, PinnedClientRefusesAServerWithoutIdentity)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	const IdentityKey key = IdentityKey::Generate();

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, client, server, &pin));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_PUBLIC_KEY_MISMATCH, 5000));
	net.Step(15000);
	EXPECT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	EXPECT_EQ(1u, net.Inbox(client).size()) << "unexpected extra message";
	ExpectClientHoldsNothing(net, client, server);
	EXPECT_FALSE(net.Received(server, ID_NEW_INCOMING_CONNECTION));
}

// Tampering with any signed or identity byte of a genuine REPLY_2 is a mismatch for a pinned client, reported
// once; the genuine reply delivered afterwards finds the attempt over.
TEST(ServerIdentity, TamperedReply2IsAMismatchForAPinnedClient)
{
	struct Region
	{
		const char *name;
		size_t (*offset)(const Reply2 &);
		uint8_t mask;
	};
	const Region regions[] = {
	    {"server crypt info public key", [](const Reply2 &r)
	     { return r.serverInfoOffset + 1 + 7; }, 0x01},
	    {"server crypt info nonce", [](const Reply2 &r)
	     { return r.serverInfoOffset + 1 + 32 + 3; }, 0x80},
	    {"identity flag (1 -> 0: no block)", [](const Reply2 &r)
	     { return r.identityOffset; }, 0x01},
	    {"identity public key, first byte", [](const Reply2 &r)
	     { return r.identityOffset + 1; }, 0x01},
	    {"identity public key, last byte", [](const Reply2 &r)
	     { return r.identityOffset + 32; }, 0x40},
	    {"signature, first byte", [](const Reply2 &r)
	     { return r.identityOffset + 33; }, 0x01},
	    {"signature, middle byte", [](const Reply2 &r)
	     { return r.identityOffset + 33 + 31; }, 0x10},
	    {"signature, last byte", [](const Reply2 &r)
	     { return r.identityOffset + 96; }, 0x02},
	};
	for (const Region &region : regions)
	{
		SCOPED_TRACE(region.name);
		SimulatedNetwork net;
		const IdentityKey key = IdentityKey::Generate();
		net.SetNewPeerIdentityKey(key.secretKey);
		RakPeer *server = net.AddPeer(1, 1);
		net.SetNewPeerIdentityKey(nullptr);
		RakPeer *client = net.AddPeer(1);

		HeldHandshake held(net, client, server);
		PublicKey pin = key.Pin();
		ASSERT_TRUE(held.RunToReply2(&pin));
		ASSERT_TRUE(held.reply2.identity.present);
		ASSERT_EQ(held.reply2.identityOffset + 97, held.reply2.bytes.size());

		std::vector<unsigned char> tampered = held.reply2.bytes;
		tampered[region.offset(held.reply2)] ^= region.mask;
		held.InjectReply2(tampered);
		ASSERT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));

		held.Release();
		held.InjectReply2(held.reply2.bytes);
		net.Step(15000);
		EXPECT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
		EXPECT_EQ(1u, net.Inbox(client).size()) << "unexpected extra message";
		ExpectClientHoldsNothing(net, client, server);
	}
}

// A man in the middle that replaces the identity block with one it signed itself -- a valid signature over the
// real transcript, under its own key -- is refused by a pinned client.
TEST(ServerIdentity, AttackerResigningTheReplyIsAMismatch)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	const IdentityKey attacker = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	HeldHandshake held(net, client, server);
	PublicKey pin = key.Pin();
	ASSERT_TRUE(held.RunToReply2(&pin));
	IdentityBlock resigned;
	ASSERT_TRUE(SignServerIdentity(attacker.secretKey, held.ClientInfo(), held.reply2.serverInfo, &resigned));
	ASSERT_TRUE(VerifyServerIdentity(resigned, held.ClientInfo(), held.reply2.serverInfo));
	held.InjectReply2(held.reply2.WithIdentity(resigned));
	EXPECT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));

	// Dropping the block altogether is no better.
	held.InjectReply2(held.reply2.WithIdentity(IdentityBlock()));
	held.Release();
	net.Step(15000);
	EXPECT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	ExpectClientHoldsNothing(net, client, server);
}

// The same substitution against a client that pinned nothing: the signature verifies, so the client records the
// attacker's key -- but the identity block is in the key schedule, so its keys differ from the server's and the
// connection never forms.
TEST(ServerIdentity, SubstitutedIdentityBlockNeverYieldsAConnection)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	const IdentityKey attacker = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	HeldHandshake held(net, client, server);
	ASSERT_TRUE(held.RunToReply2(nullptr));
	IdentityBlock resigned;
	ASSERT_TRUE(SignServerIdentity(attacker.secretKey, held.ClientInfo(), held.reply2.serverInfo, &resigned));
	held.Release();
	held.InjectReply2(held.reply2.WithIdentity(resigned));
	net.Step(3000);

	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(net.Received(server, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(0u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	EXPECT_GT(server->GetStatistics(net.AddressOf(client))->droppedUnauthenticatedDatagrams, 0u)
	    << "the client's ID_CONNECTION_REQUEST should have failed to open on the server";
}

TEST(ServerIdentity, UnpinnedClientConnectsAndReadsTheServerKey)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	ASSERT_TRUE(net.Connect(client, server));
	uint8_t remote[32];
	ASSERT_TRUE(client->GetRemoteIdentityKey(net.AddressOf(server), remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
	ExpectDataFlows(net, client, server);
}

// The identity key lives with the connection: once it closes there is nothing to read.
TEST(ServerIdentity, RemoteIdentityKeyIsGoneAfterDisconnect)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	ASSERT_TRUE(net.Connect(client, server));
	const SystemAddress serverAddress = net.AddressOf(server);
	uint8_t remote[32];
	ASSERT_TRUE(client->GetRemoteIdentityKey(serverAddress, remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));

	client->CloseConnection(serverAddress, true);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 2000));
	EXPECT_FALSE(client->GetRemoteIdentityKey(serverAddress, remote));
	EXPECT_EQ(std::vector<uint8_t>(32, 0), Bytes(remote, 32));
}

TEST(ServerIdentity, UnpinnedClientAgainstAServerWithoutKey)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	uint8_t remote[32];
	EXPECT_FALSE(client->GetRemoteIdentityKey(net.AddressOf(server), remote));
	EXPECT_EQ(std::vector<uint8_t>(32, 0), Bytes(remote, 32));
	EXPECT_FALSE(client->GetRemoteIdentityKey(UNASSIGNED_SYSTEM_ADDRESS, remote));
}

// A client that pinned nothing still verifies a present block. A bad signature drops that reply without a word
// (no ID_PUBLIC_KEY_MISMATCH to provoke), and the attempt carries on with the genuine reply.
TEST(ServerIdentity, UnpinnedClientDropsABadSignatureSilently)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	HeldHandshake held(net, client, server);
	ASSERT_TRUE(held.RunToReply2(nullptr));
	std::vector<unsigned char> tampered = held.reply2.bytes;
	tampered[held.reply2.identityOffset + 40] ^= 0x01; // a signature byte
	held.InjectReply2(tampered);
	EXPECT_TRUE(net.Inbox(client).empty());
	EXPECT_EQ(IS_PENDING, client->GetConnectionState(net.AddressOf(server)));

	held.Release();
	held.InjectReply2(held.reply2.bytes);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Connected(net, client, server); }, 3000));
	EXPECT_EQ(0u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	uint8_t remote[32];
	ASSERT_TRUE(client->GetRemoteIdentityKey(net.AddressOf(server), remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
}

// A malformed identity block (unknown flag, truncated) is a malformed REPLY_2: dropped silently, pinned or not.
TEST(ServerIdentity, MalformedIdentityBlockIsDropped)
{
	for (bool pinned : {false, true})
	{
		SCOPED_TRACE(pinned ? "pinned" : "not pinned");
		SimulatedNetwork net;
		const IdentityKey key = IdentityKey::Generate();
		net.SetNewPeerIdentityKey(key.secretKey);
		RakPeer *server = net.AddPeer(1, 1);
		net.SetNewPeerIdentityKey(nullptr);
		RakPeer *client = net.AddPeer(1);

		HeldHandshake held(net, client, server);
		PublicKey pin = key.Pin();
		ASSERT_TRUE(held.RunToReply2(pinned ? &pin : nullptr));
		std::vector<unsigned char> badFlag = held.reply2.bytes;
		badFlag[held.reply2.identityOffset] = 2;
		held.InjectReply2(badFlag);
		std::vector<unsigned char> truncated(held.reply2.bytes.begin(), held.reply2.bytes.end() - 1);
		held.InjectReply2(truncated);
		std::vector<unsigned char> noBlock(held.reply2.bytes.begin(), held.reply2.bytes.begin() + (long) held.reply2.identityOffset);
		held.InjectReply2(noBlock);
		EXPECT_TRUE(net.Inbox(client).empty());
		EXPECT_EQ(IS_PENDING, client->GetConnectionState(net.AddressOf(server)));

		held.Release();
		held.InjectReply2(held.reply2.bytes);
		ASSERT_TRUE(net.RunUntil([&]
		                         { return Connected(net, client, server); }, 3000));
	}
}

// The server answers a retransmitted REQUEST_2 with the same REPLY_2, identity block included (Ed25519 is
// deterministic), so a pinned client whose first REPLY_2 was lost still connects.
TEST(ServerIdentity, RetransmittedReply2CarriesTheSameIdentityBlock)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);

	HeldHandshake held(net, client, server);
	PublicKey pin = key.Pin();
	ASSERT_TRUE(held.RunToReply2(&pin));
	const std::vector<unsigned char> first = held.reply2.bytes;
	// IPv4 client address: 84 bytes without an identity key, plus 96 for the present block.
	EXPECT_EQ(180u, first.size());
	held.fromServer.clear();
	net.InjectDatagram(net.AddressOf(client), server, held.request2.data(), (int) held.request2.size());
	net.Step(10);
	const std::vector<unsigned char> *second = held.LastFromServer(ID_OPEN_CONNECTION_REPLY_2);
	ASSERT_NE(nullptr, second);
	EXPECT_EQ(first, *second);

	held.Release();
	held.InjectReply2(*second);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Connected(net, client, server); }, 3000));
}

TEST(ServerIdentity, PinnedConnectUnderLossAndJitter)
{
	SimulatedNetwork net(11);
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeer(1);
	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = 25;
	lossy.delayMs = 30;
	lossy.jitterMs = 40;
	net.SetLinkBothWays(client, server, lossy);

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, client, server, &pin));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return Connected(net, client, server); }, 30000));
	EXPECT_EQ(0u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
}

TEST(ServerIdentity, PlaintextModeWithIdentity)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	const IdentityKey key = IdentityKey::Generate();
	const IdentityKey wrong = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(2, 2);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *good = net.AddPeer(1);
	RakPeer *bad = net.AddPeer(1);

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, good, server, &pin));
	ASSERT_TRUE(net.RunUntilReceived(good, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	uint8_t remote[32];
	ASSERT_TRUE(good->GetRemoteIdentityKey(net.AddressOf(server), remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
	SessionKeys keys;
	EXPECT_FALSE(good->GetSessionKeysForTesting(net.AddressOf(server), &keys)) << "plaintext mode has no keys";

	PublicKey wrongPin = wrong.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, bad, server, &wrongPin));
	ASSERT_TRUE(net.RunUntilReceived(bad, ID_PUBLIC_KEY_MISMATCH, 5000));
	net.Step(15000);
	EXPECT_EQ(1u, CountOf(net, bad, ID_PUBLIC_KEY_MISMATCH));
	ExpectClientHoldsNothing(net, bad, server);
}

TEST(ServerIdentity, ConnectWithSocketPinsLikeConnect)
{
	SimulatedNetwork net;
	const IdentityKey key = IdentityKey::Generate();
	const IdentityKey wrong = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeer(2, 2);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *good = net.AddPeer(1);
	RakPeer *bad = net.AddPeer(1);
	const SystemAddress target = net.AddressOf(server);
	char host[64];
	target.ToString(false, host, sizeof(host));

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, good->ConnectWithSocket(host, target.GetPort(), nullptr, 0, good->GetSocket(UNASSIGNED_SYSTEM_ADDRESS), &pin));
	ASSERT_TRUE(net.RunUntilReceived(good, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	uint8_t remote[32];
	ASSERT_TRUE(good->GetRemoteIdentityKey(target, remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));

	PublicKey wrongPin = wrong.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, bad->ConnectWithSocket(host, target.GetPort(), nullptr, 0, bad->GetSocket(UNASSIGNED_SYSTEM_ADDRESS), &wrongPin));
	ASSERT_TRUE(net.RunUntilReceived(bad, ID_PUBLIC_KEY_MISMATCH, 5000));
	net.Step(15000);
	EXPECT_EQ(1u, CountOf(net, bad, ID_PUBLIC_KEY_MISMATCH));
	ExpectClientHoldsNothing(net, bad, server);
}

// Happy Eyeballs: every candidate carries the pin, and a mismatch is reported once, under the first address.
TEST(ServerIdentity, HappyEyeballsMismatchIsReportedOnce)
{
	SimulatedNetwork net;
	SimulatedNetwork::LinkRule slow;
	slow.delayMs = 200; // both candidates start before the first reply arrives
	net.SetDefaultLink(slow);
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.AddHostName("srv.test", {server6, server4});

	PublicKey pin = IdentityKey::Generate().Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client->Connect("srv.test", server6.GetPort(), nullptr, 0, &pin));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_PUBLIC_KEY_MISMATCH, 5000));
	net.Step(15000);
	EXPECT_EQ(1u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
	EXPECT_TRUE(net.Find(client, ID_PUBLIC_KEY_MISMATCH)->systemAddress == server6);
	EXPECT_EQ(1u, net.Inbox(client).size()) << "unexpected extra message";
	EXPECT_EQ(0u, client->NumberOfConnections());
	EXPECT_EQ(IS_NOT_CONNECTED, client->GetConnectionState(server6));
	EXPECT_EQ(IS_NOT_CONNECTED, client->GetConnectionState(server4));
}

TEST(ServerIdentity, HappyEyeballsPinnedConnect)
{
	SimulatedNetwork net;
	SimulatedNetwork::LinkRule slow;
	slow.delayMs = 200;
	net.SetDefaultLink(slow);
	const IdentityKey key = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(key.secretKey);
	RakPeer *server = net.AddPeerDualStack(4, 4);
	net.SetNewPeerIdentityKey(nullptr);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	net.AddHostName("srv.test", {server6, net.AddressOf(server, 4)});

	PublicKey pin = key.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, client->Connect("srv.test", server6.GetPort(), nullptr, 0, &pin));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	uint8_t remote[32];
	ASSERT_TRUE(client->GetRemoteIdentityKey(net.Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->systemAddress, remote));
	EXPECT_EQ(Bytes(key.publicKey, 32), Bytes(remote, 32));
	EXPECT_EQ(0u, CountOf(net, client, ID_PUBLIC_KEY_MISMATCH));
}

// ------------------------------------------------------------ simultaneous connect

namespace
{
// Two peers, each with its own identity key, that accept one incoming connection each, named by GUID order.
struct IdentityPair
{
	RakPeer *lower;
	RakPeer *higher;
	IdentityKey lowerKey;
	IdentityKey higherKey;
};

IdentityPair AddIdentityPair(SimulatedNetwork &net)
{
	// GUIDs are random and the key must be set before Startup, so both peers get a key and the GUID order
	// decides afterwards which one is "lower".
	const IdentityKey keyA = IdentityKey::Generate();
	const IdentityKey keyB = IdentityKey::Generate();
	net.SetNewPeerIdentityKey(keyA.secretKey);
	RakPeer *a = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(keyB.secretKey);
	RakPeer *b = net.AddPeer(1, 1);
	net.SetNewPeerIdentityKey(nullptr);
	return a->GetMyGUID().g < b->GetMyGUID().g ? IdentityPair{a, b, keyA, keyB} : IdentityPair{b, a, keyB, keyA};
}

bool BothConnected(RakPeer *a, const SystemAddress &aToB, RakPeer *b, const SystemAddress &bToA)
{
	return a->GetConnectionState(aToB) == IS_CONNECTED && b->GetConnectionState(bToA) == IS_CONNECTED;
}
} // namespace

// The lower GUID's handshake wins; the higher peer answers it as responder and proves its identity.
TEST(ServerIdentityCrossConnect, LowerPinsHigher)
{
	SimulatedNetwork net;
	const IdentityPair p = AddIdentityPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	PublicKey pin = p.higherKey.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.lower, p.higher, &pin));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.higher, p.lower, nullptr));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 3000));
	EXPECT_EQ(1u, CountOf(net, p.lower, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(1u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(0u, CountOf(net, p.lower, ID_PUBLIC_KEY_MISMATCH));
	uint8_t remote[32];
	ASSERT_TRUE(p.lower->GetRemoteIdentityKey(higherAddress, remote));
	EXPECT_EQ(Bytes(p.higherKey.publicKey, 32), Bytes(remote, 32));
	ExpectDataFlows(net, p.lower, p.higher);
}

// The higher peer pinned the lower one. Its attempt must not be absorbed into the lower peer's handshake (in which
// the lower peer proves nothing): it refuses that handshake with ID_ALREADY_CONNECTED and connects as
// initiator, so the lower peer has to prove the pinned identity.
TEST(ServerIdentityCrossConnect, HigherPinsLower)
{
	SimulatedNetwork net;
	const IdentityPair p = AddIdentityPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	PublicKey pin = p.lowerKey.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.lower, p.higher, nullptr));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.higher, p.lower, &pin));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 5000));
	ASSERT_TRUE(net.RunUntilReceived(p.higher, ID_CONNECTION_REQUEST_ACCEPTED, 2000));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(1u, CountOf(net, p.lower, ID_ALREADY_CONNECTED));
	EXPECT_EQ(0u, CountOf(net, p.lower, ID_NO_FREE_INCOMING_CONNECTIONS)) << "the server is not full";
	EXPECT_EQ(1u, CountOf(net, p.lower, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_PUBLIC_KEY_MISMATCH));
	uint8_t remote[32];
	ASSERT_TRUE(p.higher->GetRemoteIdentityKey(lowerAddress, remote));
	EXPECT_EQ(Bytes(p.lowerKey.publicKey, 32), Bytes(remote, 32));
	ExpectDataFlows(net, p.lower, p.higher);
}

// The higher peer pinned a key the lower peer does not hold: a mismatch, and no connection at all.
TEST(ServerIdentityCrossConnect, HigherPinsWrongKey)
{
	SimulatedNetwork net;
	const IdentityPair p = AddIdentityPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	PublicKey pin = IdentityKey::Generate().Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.lower, p.higher, nullptr));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.higher, p.lower, &pin));
	ASSERT_TRUE(net.RunUntilReceived(p.higher, ID_PUBLIC_KEY_MISMATCH, 5000));
	net.Step(15000);
	EXPECT_EQ(1u, CountOf(net, p.higher, ID_PUBLIC_KEY_MISMATCH));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_NEW_INCOMING_CONNECTION));
	EXPECT_NE(IS_CONNECTED, p.higher->GetConnectionState(lowerAddress));
	EXPECT_NE(IS_CONNECTED, p.lower->GetConnectionState(higherAddress));
}

// Both pinned each other: the higher peer's refusal ends the lower peer's attempt, and the higher peer connects
// as initiator with the lower peer's identity proven.
TEST(ServerIdentityCrossConnect, BothPin)
{
	SimulatedNetwork net;
	const IdentityPair p = AddIdentityPair(net);
	const SystemAddress lowerAddress = net.AddressOf(p.lower);
	const SystemAddress higherAddress = net.AddressOf(p.higher);

	PublicKey pinHigher = p.higherKey.Pin();
	PublicKey pinLower = p.lowerKey.Pin();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.lower, p.higher, &pinHigher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, ConnectTo(net, p.higher, p.lower, &pinLower));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return BothConnected(p.lower, higherAddress, p.higher, lowerAddress); }, 5000));
	ASSERT_TRUE(net.RunUntilReceived(p.higher, ID_CONNECTION_REQUEST_ACCEPTED, 2000));
	EXPECT_EQ(1u, CountOf(net, p.lower, ID_ALREADY_CONNECTED));
	EXPECT_EQ(0u, CountOf(net, p.lower, ID_NO_FREE_INCOMING_CONNECTIONS));
	EXPECT_EQ(0u, CountOf(net, p.lower, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(0u, CountOf(net, p.lower, ID_PUBLIC_KEY_MISMATCH));
	EXPECT_EQ(0u, CountOf(net, p.higher, ID_PUBLIC_KEY_MISMATCH));
	uint8_t remote[32];
	ASSERT_TRUE(p.higher->GetRemoteIdentityKey(lowerAddress, remote));
	EXPECT_EQ(Bytes(p.lowerKey.publicKey, 32), Bytes(remote, 32));
	// Only one pin is enforced: the lower peer's Connect ended, and it answered the higher peer's handshake as
	// responder, so it proved nothing about the higher peer.
	EXPECT_FALSE(p.lower->GetRemoteIdentityKey(higherAddress, remote));
	ExpectDataFlows(net, p.lower, p.higher);
}
