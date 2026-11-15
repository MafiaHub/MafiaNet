/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The rejection paths of the encrypted connection handshake: a REPLY_2 in the
 *  other encryption mode, a low-order X25519 key on either side, and REPLY_1,
 *  REPLY_2 and REQUEST_2 that are cut short at every byte. Each test forges the
 *  datagram, injects it on the SimulatedNetwork, and checks both what the peer
 *  does (the message it reports, if any) and what it must not do: allocate a
 *  connection, derive keys, or answer.
 *
 *  See docs/advanced/encryption-wire-format.rst for the layouts.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/handshake_challenge.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/session_crypto.h"

using namespace MafiaNet;

namespace
{
const RakNetGUID kForgedServerGuid(0x5E5E5E5E00000001ull);

void WriteOfflineHeader(BitStream &out, MessageID id)
{
	out.Write(id);
	out.WriteAlignedBytes(RakPeer::GetOfflineMessageDataIdForTesting(), RakPeer::kOfflineMessageDataIdBytes);
}

// id | offline id (16) | server guid (8) | challenge (8) | mtu (2): 35 bytes.
std::vector<unsigned char> Reply1(uint64_t challenge, uint16_t mtu)
{
	BitStream out;
	WriteOfflineHeader(out, ID_OPEN_CONNECTION_REPLY_1);
	out.Write(kForgedServerGuid);
	out.Write(challenge);
	out.Write(mtu);
	return std::vector<unsigned char>(out.GetData(), out.GetData() + out.GetNumberOfBytesUsed());
}

// id | offline id (16) | nonce echo (8) | server guid (8) | client address | mtu (2) | server crypt info (41).
std::vector<unsigned char> Reply2(uint64_t nonceEcho, const SystemAddress &clientAddress, uint16_t mtu, const CryptInfo &serverInfo)
{
	BitStream out;
	WriteOfflineHeader(out, ID_OPEN_CONNECTION_REPLY_2);
	out.Write(nonceEcho);
	out.Write(kForgedServerGuid);
	out.Write(clientAddress);
	out.Write(mtu);
	serverInfo.Serialize(&out);
	return std::vector<unsigned char>(out.GetData(), out.GetData() + out.GetNumberOfBytesUsed());
}

// id | offline id (16) | challenge (8) | client crypt info (41) | binding address | mtu (2) | client guid (8).
std::vector<unsigned char> Request2(uint64_t challenge, const CryptInfo &clientInfo, const SystemAddress &bindingAddress,
                                    uint16_t mtu, const RakNetGUID &guid)
{
	BitStream out;
	WriteOfflineHeader(out, ID_OPEN_CONNECTION_REQUEST_2);
	out.Write(challenge);
	clientInfo.Serialize(&out);
	out.Write(bindingAddress);
	out.Write(mtu);
	out.Write(guid);
	return std::vector<unsigned char>(out.GetData(), out.GetData() + out.GetNumberOfBytesUsed());
}

// Byte offsets of the fields, for the truncation tests and to pin the layouts the helpers write.
const size_t kHeaderBytes = 1 + RakPeer::kOfflineMessageDataIdBytes; // 17
const size_t kReply1Bytes = kHeaderBytes + 8 + 8 + 2;                // 35
const size_t kIpv4AddressBytes = 7;
const size_t kReply2Bytes = kHeaderBytes + 8 + 8 + kIpv4AddressBytes + 2 + CryptInfo::kSerializedBytes;   // 83
const size_t kRequest2Bytes = kHeaderBytes + 8 + CryptInfo::kSerializedBytes + kIpv4AddressBytes + 2 + 8; // 83

size_t CountOf(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	size_t n = 0;
	for (const auto &message : net.Inbox(peer))
		n += message.id == id;
	return n;
}

// Counts every datagram one address sends, by its first byte.
struct SendCounter
{
	SystemAddress sender;
	std::vector<std::vector<char>> sent;

	void Attach(SimulatedNetwork &net, const SystemAddress &who)
	{
		sender = who;
		net.SetTap([this](const SystemAddress &from, const SystemAddress &, const char *data, int length)
		           {
			           if (from == sender)
				           sent.emplace_back(data, data + length); });
	}
	size_t Count(MessageID id) const
	{
		size_t n = 0;
		for (const auto &d : sent)
			n += !d.empty() && (unsigned char) d[0] == id;
		return n;
	}
	// Datagrams of a connection rather than offline messages: a sealed datagram's prefix and a plaintext
	// reliability-layer header both have bit 7 set; every offline message ID is below 0x80.
	size_t ConnectedDatagrams() const
	{
		size_t n = 0;
		for (const auto &d : sent)
			n += !d.empty() && ((unsigned char) d[0] & 0x80) != 0;
		return n;
	}
};

// A client with a connection attempt pending to an address no peer owns, so every REPLY_1 or REPLY_2 it
// gets is one the test injects. The attempt has been started and its first REQUEST_1 sent.
struct PendingAttempt
{
	RakPeer *client = nullptr;
	SystemAddress clientAddress;
	SystemAddress target;
	uint64_t nonce = 0;
};

PendingAttempt StartAttemptToNowhere(SimulatedNetwork &net)
{
	PendingAttempt attempt;
	attempt.client = net.AddPeer(1);
	attempt.clientAddress = net.AddressOf(attempt.client);
	attempt.target = SystemAddress("10.0.0.250", 5000);
	EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, attempt.client->Connect("10.0.0.250", 5000, nullptr, 0));
	net.Step(10);
	EXPECT_TRUE(attempt.client->GetPendingHandshakeNonceForTesting(attempt.target, &attempt.nonce));
	return attempt;
}

void Inject(SimulatedNetwork &net, const SystemAddress &from, const RakPeer *to, const std::vector<unsigned char> &bytes, size_t length)
{
	ASSERT_LE(length, bytes.size());
	net.InjectDatagram(from, to, bytes.data(), (int) length);
}

CryptInfo PlaintextInfo(uint64_t nonce)
{
	CryptInfo info;
	info.mode = EncryptionMode::Plaintext;
	info.nonce = nonce;
	return info;
}

// A mode Required crypt info carrying a given (low-order) public key.
CryptInfo RequiredInfoWithKey(const uint8_t key[32], uint64_t nonce)
{
	CryptInfo info;
	info.mode = EncryptionMode::Required;
	std::memcpy(info.publicKey, key, sizeof(info.publicKey));
	info.nonce = nonce;
	return info;
}

// Points of small order on Curve25519 (u-coordinates, little-endian): X25519 of any clamped scalar with
// them is all zero: 0 (order 2), 1 (order 4) and a point of order 8.
const uint8_t kLowOrderZero[32] = {};
const uint8_t kLowOrderOne[32] = {1};
const uint8_t kLowOrderEight[32] = {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
                                    0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00};

// After the attempt was refused: it is gone (no longer pending, no REQUEST_1 retries), no keys exist for the
// target, and nothing else was reported for a long time after.
void ExpectAttemptRetired(SimulatedNetwork &net, const PendingAttempt &attempt, const SendCounter &sends)
{
	SessionKeys keys;
	EXPECT_FALSE(attempt.client->GetSessionKeysForTesting(attempt.target, &keys));
	EXPECT_EQ(IS_NOT_CONNECTED, attempt.client->GetConnectionState(attempt.target));
	uint64_t nonce = 0;
	EXPECT_FALSE(attempt.client->GetPendingHandshakeNonceForTesting(attempt.target, &nonce)) << "the attempt is still queued";
	EXPECT_EQ(0u, attempt.client->NumberOfConnections());

	const size_t sentBefore = sends.sent.size();
	net.Step(10000); // longer than a whole connection attempt
	EXPECT_EQ(sentBefore, sends.sent.size()) << "the client kept sending to the refused address";
	EXPECT_EQ(1u, net.Inbox(attempt.client).size()) << "more than the one refusal was reported";
	EXPECT_EQ(0u, CountOf(net, attempt.client, ID_CONNECTION_REQUEST_ACCEPTED));
}

// --- Path 1: a REPLY_2 in the other encryption mode --------------------------------------------------

void RunModeMismatchReply2(EncryptionMode clientMode, const CryptInfo &serverInfo, MessageID expected, MessageID notExpected)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(clientMode);
	PendingAttempt attempt = StartAttemptToNowhere(net);
	SendCounter sends;
	sends.Attach(net, attempt.clientAddress);

	const std::vector<unsigned char> reply2 = Reply2(attempt.nonce, attempt.clientAddress, 1200, serverInfo);
	ASSERT_EQ(kReply2Bytes, reply2.size());
	// Twice: the attempt is dropped by the first, so the second matches nothing.
	Inject(net, attempt.target, attempt.client, reply2, reply2.size());
	Inject(net, attempt.target, attempt.client, reply2, reply2.size());
	net.Step(20);

	ASSERT_EQ(1u, CountOf(net, attempt.client, expected));
	const SimulatedNetwork::Message *message = net.Find(attempt.client, expected);
	EXPECT_EQ(attempt.target, message->systemAddress);
	EXPECT_EQ(kForgedServerGuid, message->guid);
	EXPECT_EQ(0u, CountOf(net, attempt.client, notExpected));
	EXPECT_EQ(0u, CountOf(net, attempt.client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(0u, sends.ConnectedDatagrams()) << "the client went on with the handshake";
	ExpectAttemptRetired(net, attempt, sends);
}

// --- Path 2: a REPLY_2 whose server key is low order -------------------------------------------------

void RunLowOrderServerKey(const uint8_t key[32])
{
	SimulatedNetwork net;
	PendingAttempt attempt = StartAttemptToNowhere(net);
	SendCounter sends;
	sends.Attach(net, attempt.clientAddress);

	const std::vector<unsigned char> reply2 = Reply2(attempt.nonce, attempt.clientAddress, 1200, RequiredInfoWithKey(key, 0x1234));
	Inject(net, attempt.target, attempt.client, reply2, reply2.size());
	Inject(net, attempt.target, attempt.client, reply2, reply2.size());
	net.Step(20);

	ASSERT_EQ(1u, CountOf(net, attempt.client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(attempt.target, net.Find(attempt.client, ID_CONNECTION_ATTEMPT_FAILED)->systemAddress);
	EXPECT_EQ(0u, sends.ConnectedDatagrams()) << "the client sealed with keys from a low-order point";
	ExpectAttemptRetired(net, attempt, sends);
}

// --- Path 3: a REQUEST_2 with a valid challenge and a low-order client key ---------------------------

void RunLowOrderClientKey(const uint8_t key[32])
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	const SystemAddress serverAddress = net.AddressOf(server);
	SendCounter sends;
	sends.Attach(net, serverAddress);

	const SystemAddress sender("10.0.0.91", 6000);
	const std::vector<unsigned char> request = Request2(server->GetHandshakeChallengeForTesting().Generate(net.Now(), sender),
	                                                    RequiredInfoWithKey(key, 0xC0FFEE), serverAddress, 1200, RakNetGUID(0xA1));
	ASSERT_EQ(kRequest2Bytes, request.size());
	Inject(net, sender, server, request, request.size());
	net.Step(50);

	// The challenge verified and the key exchange ran: the drop is the low-order key's, not the challenge's.
	EXPECT_EQ(1u, server->GetServerKeyExchangeCountForTesting());
	EXPECT_TRUE(sends.sent.empty()) << "the server answered a REQUEST_2 with a low-order key";
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(sender));
	EXPECT_EQ(0u, server->NumberOfConnections());
	SessionKeys keys;
	EXPECT_FALSE(server->GetSessionKeysForTesting(sender, &keys));
	EXPECT_TRUE(net.Inbox(server).empty());

	// Control: the same request with a real key is answered and allocated.
	EphemeralKeyPair pair;
	const std::vector<unsigned char> good = Request2(server->GetHandshakeChallengeForTesting().Generate(net.Now(), sender),
	                                                 pair.MakeCryptInfo(), serverAddress, 1200, RakNetGUID(0xA1));
	Inject(net, sender, server, good, good.size());
	net.Step(10);
	EXPECT_EQ(1u, sends.Count(ID_OPEN_CONNECTION_REPLY_2));
	EXPECT_NE(IS_NOT_CONNECTED, server->GetConnectionState(sender));
}
} // namespace

TEST(EncryptedHandshakeRejection, RequiredClientRefusesAPlaintextReply2)
{
	RunModeMismatchReply2(EncryptionMode::Required, PlaintextInfo(0), ID_OUR_SYSTEM_REQUIRES_SECURITY,
	                      ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION);
}

TEST(EncryptedHandshakeRejection, PlaintextClientRefusesARequiredReply2)
{
	EphemeralKeyPair serverPair;
	RunModeMismatchReply2(EncryptionMode::Plaintext, serverPair.MakeCryptInfo(), ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION,
	                      ID_OUR_SYSTEM_REQUIRES_SECURITY);
}

TEST(EncryptedHandshakeRejection, Reply2WithAllZeroServerKeyFailsTheAttempt)
{
	RunLowOrderServerKey(kLowOrderZero);
}

TEST(EncryptedHandshakeRejection, Reply2WithOrderFourServerKeyFailsTheAttempt)
{
	RunLowOrderServerKey(kLowOrderOne);
}

TEST(EncryptedHandshakeRejection, Reply2WithOrderEightServerKeyFailsTheAttempt)
{
	RunLowOrderServerKey(kLowOrderEight);
}

TEST(EncryptedHandshakeRejection, Request2WithAllZeroClientKeyIsDroppedSilently)
{
	RunLowOrderClientKey(kLowOrderZero);
}

TEST(EncryptedHandshakeRejection, Request2WithOrderEightClientKeyIsDroppedSilently)
{
	RunLowOrderClientKey(kLowOrderEight);
}

// --- Path 4: handshake messages cut short --------------------------------------------------------------

// A REPLY_1 cut anywhere before its last byte does not make the client send REQUEST_2.
TEST(EncryptedHandshakeRejection, TruncatedReply1IsDropped)
{
	SimulatedNetwork net;
	PendingAttempt attempt = StartAttemptToNowhere(net);
	SendCounter sends;
	sends.Attach(net, attempt.clientAddress);

	const uint64_t challenge = 0x0102030405060708ull;
	const std::vector<unsigned char> reply1 = Reply1(challenge, 1200);
	ASSERT_EQ(kReply1Bytes, reply1.size());
	for (size_t length = 1; length < reply1.size(); ++length)
	{
		Inject(net, attempt.target, attempt.client, reply1, length);
		net.Step(1);
	}
	net.Step(20);
	EXPECT_EQ(0u, sends.Count(ID_OPEN_CONNECTION_REQUEST_2)) << "a truncated REPLY_1 was answered";
	EXPECT_EQ(IS_PENDING, attempt.client->GetConnectionState(attempt.target));
	EXPECT_TRUE(net.Inbox(attempt.client).empty());

	// Control: the whole REPLY_1 is answered, echoing its challenge.
	Inject(net, attempt.target, attempt.client, reply1, reply1.size());
	net.Step(10);
	ASSERT_EQ(1u, sends.Count(ID_OPEN_CONNECTION_REQUEST_2));
	for (const auto &d : sends.sent)
	{
		if (d.empty() || (unsigned char) d[0] != ID_OPEN_CONNECTION_REQUEST_2)
			continue;
		BitStream in(reinterpret_cast<unsigned char *>(const_cast<char *>(d.data())), (unsigned int) d.size(), false);
		in.IgnoreBytes(kHeaderBytes);
		uint64_t echoed = 0;
		ASSERT_TRUE(in.Read(echoed));
		EXPECT_EQ(challenge, echoed);
	}
}

// A REPLY_2 that echoes the right nonce but is cut anywhere before its last byte derives nothing, reports
// nothing, and leaves the attempt pending.
TEST(EncryptedHandshakeRejection, TruncatedReply2IsDropped)
{
	SimulatedNetwork net;
	PendingAttempt attempt = StartAttemptToNowhere(net);
	SendCounter sends;
	sends.Attach(net, attempt.clientAddress);

	EphemeralKeyPair serverPair;
	const std::vector<unsigned char> reply2 = Reply2(attempt.nonce, attempt.clientAddress, 1200, serverPair.MakeCryptInfo());
	ASSERT_EQ(kReply2Bytes, reply2.size());
	for (size_t length = 1; length < reply2.size(); ++length)
	{
		Inject(net, attempt.target, attempt.client, reply2, length);
		net.Step(1);
	}
	net.Step(20);

	SessionKeys keys;
	EXPECT_FALSE(attempt.client->GetSessionKeysForTesting(attempt.target, &keys)) << "keys from a truncated REPLY_2";
	EXPECT_EQ(IS_PENDING, attempt.client->GetConnectionState(attempt.target));
	EXPECT_TRUE(net.Inbox(attempt.client).empty());
	EXPECT_EQ(0u, sends.ConnectedDatagrams());

	// Control: the whole REPLY_2 is taken.
	Inject(net, attempt.target, attempt.client, reply2, reply2.size());
	net.Step(10);
	EXPECT_TRUE(attempt.client->GetSessionKeysForTesting(attempt.target, &keys));
	EXPECT_GE(sends.ConnectedDatagrams(), 1u) << "no sealed ID_CONNECTION_REQUEST followed the whole REPLY_2";
}

// A whole-length REPLY_2 whose crypt info has a mode byte that is neither 0 nor 1 is malformed.
TEST(EncryptedHandshakeRejection, Reply2WithUnknownCryptInfoModeIsDropped)
{
	SimulatedNetwork net;
	PendingAttempt attempt = StartAttemptToNowhere(net);
	EphemeralKeyPair serverPair;
	std::vector<unsigned char> reply2 = Reply2(attempt.nonce, attempt.clientAddress, 1200, serverPair.MakeCryptInfo());
	const size_t modeOffset = reply2.size() - CryptInfo::kSerializedBytes;
	ASSERT_EQ(0u, reply2[modeOffset]);
	for (unsigned char mode : {2, 0x7F, 0x80, 0xFF})
	{
		reply2[modeOffset] = mode;
		Inject(net, attempt.target, attempt.client, reply2, reply2.size());
	}
	net.Step(20);

	SessionKeys keys;
	EXPECT_FALSE(attempt.client->GetSessionKeysForTesting(attempt.target, &keys));
	EXPECT_EQ(IS_PENDING, attempt.client->GetConnectionState(attempt.target));
	EXPECT_TRUE(net.Inbox(attempt.client).empty()) << "an unknown mode was reported as a mode mismatch";
}

// A REQUEST_2 with a valid challenge but cut anywhere before its last byte allocates nothing, runs no key
// exchange and gets no answer.
TEST(EncryptedHandshakeRejection, TruncatedRequest2IsDropped)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	const SystemAddress serverAddress = net.AddressOf(server);
	SendCounter sends;
	sends.Attach(net, serverAddress);

	const SystemAddress sender("10.0.0.92", 6000);
	EphemeralKeyPair pair;
	const std::vector<unsigned char> request = Request2(server->GetHandshakeChallengeForTesting().Generate(net.Now(), sender),
	                                                    pair.MakeCryptInfo(), serverAddress, 1200, RakNetGUID(0xB2));
	ASSERT_EQ(kRequest2Bytes, request.size());
	for (size_t length = 1; length < request.size(); ++length)
	{
		Inject(net, sender, server, request, length);
		net.Step(1);
	}
	net.Step(20);

	EXPECT_TRUE(sends.sent.empty()) << "the server answered a truncated REQUEST_2";
	EXPECT_EQ(0u, server->GetServerKeyExchangeCountForTesting());
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(sender));
	EXPECT_EQ(0u, server->NumberOfConnections());
	EXPECT_TRUE(net.Inbox(server).empty());

	// Control: the whole REQUEST_2, with the same (still valid) challenge, is answered and allocated.
	Inject(net, sender, server, request, request.size());
	net.Step(10);
	EXPECT_EQ(1u, sends.Count(ID_OPEN_CONNECTION_REPLY_2));
	EXPECT_NE(IS_NOT_CONNECTED, server->GetConnectionState(sender));
}

// A whole-length REQUEST_2 with a valid challenge and an unknown crypt info mode is malformed: it is not
// a mode mismatch (which would be answered), it is dropped.
TEST(EncryptedHandshakeRejection, Request2WithUnknownCryptInfoModeIsDropped)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	const SystemAddress serverAddress = net.AddressOf(server);
	SendCounter sends;
	sends.Attach(net, serverAddress);

	const SystemAddress sender("10.0.0.93", 6000);
	EphemeralKeyPair pair;
	std::vector<unsigned char> request = Request2(server->GetHandshakeChallengeForTesting().Generate(net.Now(), sender),
	                                              pair.MakeCryptInfo(), serverAddress, 1200, RakNetGUID(0xB3));
	const size_t modeOffset = kHeaderBytes + 8;
	ASSERT_EQ(0u, request[modeOffset]);
	for (unsigned char mode : {2, 0x7F, 0x80, 0xFF})
	{
		request[modeOffset] = mode;
		Inject(net, sender, server, request, request.size());
	}
	net.Step(20);

	EXPECT_TRUE(sends.sent.empty()) << "the server answered a REQUEST_2 with an unknown mode";
	EXPECT_EQ(0u, server->GetServerKeyExchangeCountForTesting());
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(sender));
}

// A REQUEST_2 whose binding address has an unknown IP version byte is malformed too.
TEST(EncryptedHandshakeRejection, Request2WithUnknownAddressVersionIsDropped)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	const SystemAddress serverAddress = net.AddressOf(server);
	SendCounter sends;
	sends.Attach(net, serverAddress);

	const SystemAddress sender("10.0.0.94", 6000);
	EphemeralKeyPair pair;
	std::vector<unsigned char> request = Request2(server->GetHandshakeChallengeForTesting().Generate(net.Now(), sender),
	                                              pair.MakeCryptInfo(), serverAddress, 1200, RakNetGUID(0xB4));
	const size_t versionOffset = kHeaderBytes + 8 + CryptInfo::kSerializedBytes;
	ASSERT_EQ(4u, request[versionOffset]);
	for (unsigned char version : {0, 5, 0xFF})
	{
		request[versionOffset] = version;
		Inject(net, sender, server, request, request.size());
	}
	net.Step(20);

	EXPECT_TRUE(sends.sent.empty());
	EXPECT_EQ(0u, server->GetServerKeyExchangeCountForTesting());
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(sender));
}
