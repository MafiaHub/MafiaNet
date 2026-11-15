/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The encrypted connection handshake (stateless challenge, ephemeral X25519
 *  crypt info in ID_OPEN_CONNECTION_REQUEST_2 / REPLY_2, session keys installed
 *  in both reliability layers) and the encryption-mode negotiation, run on the
 *  in-process SimulatedNetwork. See Tests/Support/SimulatedNetwork.h.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/handshake_challenge.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_black_hole.h"
#include "mafianet/mtu_size.h"
#include "mafianet/session_crypto.h"
#include "mafianet/statistics.h"
#include "mafianet/version.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_TEST_PAYLOAD = ID_USER_PACKET_ENUM;

// The marker every offline handshake message carries, as the peer itself defines it.
const unsigned char *const kOfflineMessageDataId = RakPeer::GetOfflineMessageDataIdForTesting();
constexpr size_t kOfflineMessageDataIdSize = RakPeer::kOfflineMessageDataIdBytes;

// A 64-byte payload no ciphertext will reproduce by chance.
std::vector<unsigned char> Marker()
{
	std::vector<unsigned char> marker(64);
	for (size_t i = 0; i < marker.size(); ++i)
		marker[i] = (unsigned char) (0x41 + (i * 7) % 26);
	return marker;
}

void SendMarker(RakPeer *from, const SystemAddress &to)
{
	BitStream bs;
	bs.Write(ID_TEST_PAYLOAD);
	const std::vector<unsigned char> marker = Marker();
	bs.WriteAlignedBytes(marker.data(), (unsigned int) marker.size());
	ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, to, false));
}

void SendNumbered(RakPeer *from, const SystemAddress &to, uint32_t number, size_t payloadBytes = 16)
{
	BitStream bs;
	bs.Write(ID_TEST_PAYLOAD);
	bs.Write(number);
	for (size_t i = 0; i < payloadBytes; ++i)
		bs.Write((unsigned char) (number + i));
	ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, to, false));
}

std::vector<uint32_t> NumbersReceived(SimulatedNetwork &net, const RakPeer *peer)
{
	std::vector<uint32_t> numbers;
	for (const auto &message : net.Inbox(peer))
	{
		if (message.id != ID_TEST_PAYLOAD || message.data.size() < 5)
			continue;
		BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
		bs.IgnoreBytes(1);
		uint32_t number = 0;
		bs.Read(number);
		numbers.push_back(number);
	}
	return numbers;
}

size_t CountOf(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	size_t n = 0;
	for (const auto &message : net.Inbox(peer))
		n += message.id == id;
	return n;
}

bool Contains(const std::vector<char> &haystack, const std::vector<unsigned char> &needle)
{
	return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
	                   [](char a, unsigned char b)
	                   { return (unsigned char) a == b; }) != haystack.end();
}

// Every datagram on the wire, in order.
struct Capture
{
	struct Datagram
	{
		SystemAddress from;
		SystemAddress to;
		std::vector<char> bytes;
	};
	std::vector<Datagram> datagrams;

	void Attach(SimulatedNetwork &net)
	{
		net.SetTap([this](const SystemAddress &from, const SystemAddress &to, const char *data, int length)
		           { datagrams.push_back({from, to, std::vector<char>(data, data + length)}); });
	}
	bool AnyContains(const std::vector<unsigned char> &needle) const
	{
		for (const Datagram &d : datagrams)
			if (Contains(d.bytes, needle))
				return true;
		return false;
	}
};

// ID_OPEN_CONNECTION_REQUEST_2, as a client writes it:
// id | offline id (16) | challenge (8) | crypt info (41) | binding address | mtu (2) | guid (8).
void WriteRequest2(BitStream &out, uint64_t challenge, const CryptInfo &info, const SystemAddress &bindingAddress,
                   uint16_t mtu, const RakNetGUID &guid)
{
	out.Write((MessageID) ID_OPEN_CONNECTION_REQUEST_2);
	out.WriteAlignedBytes(kOfflineMessageDataId, kOfflineMessageDataIdSize);
	out.Write(challenge);
	info.Serialize(&out);
	out.Write(bindingAddress);
	out.Write(mtu);
	out.Write(guid);
}

bool KeysOf(RakPeer *peer, const SystemAddress &remote, SessionKeys *out)
{
	return peer->GetSessionKeysForTesting(remote, out);
}
} // namespace

TEST(EncryptedHandshake, DefaultPeersConnectEncrypted)
{
	SimulatedNetwork net;
	Capture capture;
	capture.Attach(net);
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(1);
	EXPECT_EQ(EncryptionMode::Required, client->GetEncryptionMode());
	EXPECT_EQ(EncryptionMode::Required, server->GetEncryptionMode());

	ASSERT_TRUE(net.Connect(client, server, 5000));
	const SystemAddress serverAddress = net.AddressOf(server);
	const SystemAddress clientAddress = net.AddressOf(client);

	SessionKeys clientKeys, serverKeys;
	ASSERT_TRUE(KeysOf(client, serverAddress, &clientKeys));
	ASSERT_TRUE(KeysOf(server, clientAddress, &serverKeys));
	// The initiator's send key is the responder's receive key, and the other way round.
	EXPECT_EQ(0, std::memcmp(clientKeys.send, serverKeys.recv, sizeof(clientKeys.send)));
	EXPECT_EQ(0, std::memcmp(clientKeys.recv, serverKeys.send, sizeof(clientKeys.recv)));
	EXPECT_NE(0, std::memcmp(clientKeys.send, clientKeys.recv, sizeof(clientKeys.send)));

	SendMarker(client, serverAddress);
	SendMarker(server, clientAddress);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return net.Received(server, ID_TEST_PAYLOAD) && net.Received(client, ID_TEST_PAYLOAD); }, 2000));
	const std::vector<unsigned char> marker = Marker();
	EXPECT_EQ(0, std::memcmp(net.Find(server, ID_TEST_PAYLOAD)->data.data() + 1, marker.data(), marker.size()));
	EXPECT_FALSE(capture.AnyContains(marker)) << "the payload crossed the wire in clear";

	EXPECT_EQ(0u, server->GetStatistics(clientAddress)->droppedUnauthenticatedDatagrams);
	EXPECT_EQ(0u, client->GetStatistics(serverAddress)->droppedUnauthenticatedDatagrams);
}

TEST(EncryptedHandshake, PlaintextPeersConnect)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	Capture capture;
	capture.Attach(net);
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(1);
	EXPECT_EQ(EncryptionMode::Plaintext, client->GetEncryptionMode());

	ASSERT_TRUE(net.Connect(client, server, 5000));
	SessionKeys keys;
	EXPECT_FALSE(KeysOf(client, net.AddressOf(server), &keys));
	EXPECT_FALSE(KeysOf(server, net.AddressOf(client), &keys));

	SendMarker(client, net.AddressOf(server));
	ASSERT_TRUE(net.RunUntilReceived(server, ID_TEST_PAYLOAD, 2000));
	EXPECT_TRUE(capture.AnyContains(Marker()));
}

TEST(EncryptedHandshake, ClientPlaintextServerRequiredIsRefused)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(4, 4);
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	RakPeer *client = net.AddPeer(1);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION, 5000));
	EXPECT_EQ(server->GetMyGUID(), net.Find(client, ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION)->guid);
	EXPECT_EQ(net.AddressOf(server), net.Find(client, ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION)->systemAddress);
	net.Step(3000);

	EXPECT_EQ(1u, CountOf(net, client, ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION));
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(0u, server->NumberOfConnections());
	EXPECT_FALSE(net.Received(server, ID_NEW_INCOMING_CONNECTION));
	EXPECT_TRUE(net.Inbox(server).empty());
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(net.AddressOf(client)));
	EXPECT_EQ(IS_NOT_CONNECTED, client->GetConnectionState(net.AddressOf(server)));
}

TEST(EncryptedHandshake, ClientRequiredServerPlaintextIsRefused)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	RakPeer *server = net.AddPeer(4, 4);
	net.SetNewPeerEncryptionMode(EncryptionMode::Required);
	RakPeer *client = net.AddPeer(1);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_OUR_SYSTEM_REQUIRES_SECURITY, 5000));
	EXPECT_EQ(server->GetMyGUID(), net.Find(client, ID_OUR_SYSTEM_REQUIRES_SECURITY)->guid);
	net.Step(3000);

	EXPECT_EQ(1u, CountOf(net, client, ID_OUR_SYSTEM_REQUIRES_SECURITY));
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(0u, server->NumberOfConnections());
	EXPECT_TRUE(net.Inbox(server).empty());
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(net.AddressOf(client)));
}

TEST(EncryptedHandshake, ConnectsUnderLossJitterAndReordering)
{
	SimulatedNetwork net(7);
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = 20;
	lossy.delayMs = 30;
	lossy.jitterMs = 40;
	net.SetLinkBothWays(client, server, lossy);

	ASSERT_TRUE(net.Connect(client, server, 30000));
	SessionKeys keys;
	ASSERT_TRUE(KeysOf(server, net.AddressOf(client), &keys));

	const uint32_t kMessages = 200;
	for (uint32_t i = 0; i < kMessages; ++i)
		SendNumbered(client, net.AddressOf(server), i, 40);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return NumbersReceived(net, server).size() == kMessages; }, 60000));
	const std::vector<uint32_t> numbers = NumbersReceived(net, server);
	for (uint32_t i = 0; i < kMessages; ++i)
		ASSERT_EQ(i, numbers[i]) << "out of order at " << i;
	EXPECT_EQ(0u, server->GetStatistics(net.AddressOf(client))->droppedUnauthenticatedDatagrams);
}

TEST(EncryptedHandshake, InjectedForgedDatagramsDoNotBreakConnection)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const SystemAddress clientAddress = net.AddressOf(client);
	const SystemAddress serverAddress = net.AddressOf(server);

	uint32_t lcg = 12345;
	auto next = [&lcg]
	{
		lcg = lcg * 1664525u + 1013904223u;
		return lcg >> 8;
	};
	for (int i = 0; i < 500; ++i)
	{
		std::vector<unsigned char> datagram(1 + next() % 1200);
		for (unsigned char &b : datagram)
			b = (unsigned char) next();
		datagram[0] |= 0x80; // a connected datagram, not an offline message
		net.InjectDatagram(clientAddress, server, datagram.data(), (int) datagram.size());
		if (i % 50 == 0)
			net.Step(1);
	}
	net.Step(100);

	EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(clientAddress));
	EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(serverAddress));
	EXPECT_GT(server->GetStatistics(clientAddress)->droppedUnauthenticatedDatagrams, 0u);
	EXPECT_FALSE(net.Received(server, ID_CONNECTION_LOST));
	EXPECT_FALSE(net.Received(server, ID_DISCONNECTION_NOTIFICATION));

	SendNumbered(client, serverAddress, 1);
	SendNumbered(server, clientAddress, 2);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return NumbersReceived(net, server).size() == 1 && NumbersReceived(net, client).size() == 1; }, 2000));
	EXPECT_EQ(std::vector<uint32_t>{1}, NumbersReceived(net, server));
	EXPECT_EQ(std::vector<uint32_t>{2}, NumbersReceived(net, client));
}

TEST(EncryptedHandshake, Request2WithoutValidChallengeAllocatesNothing)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	EphemeralKeyPair pair;
	const CryptInfo info = pair.MakeCryptInfo();

	std::vector<SystemAddress> forgers;
	uint64_t challenge = 0x9E3779B97F4A7C15ull;
	for (int i = 0; i < 1000; ++i)
	{
		char host[32];
		std::snprintf(host, sizeof(host), "10.%d.%d.%d", 100 + i / 65536, (i / 256) % 256, i % 256);
		const SystemAddress forger(host, (unsigned short) (1024 + i));
		forgers.push_back(forger);
		challenge = challenge * 6364136223846793005ull + 1442695040888963407ull;
		BitStream request;
		WriteRequest2(request, challenge, info, net.AddressOf(server), 1200, RakNetGUID(0x1000 + (uint64_t) i));
		net.InjectDatagram(forger, server, request.GetData(), (int) request.GetNumberOfBytesUsed());
	}
	net.Step(200);

	EXPECT_EQ(0u, server->NumberOfConnections());
	for (const SystemAddress &forger : forgers)
		ASSERT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(forger)) << forger.ToString();
	EXPECT_TRUE(net.Inbox(server).empty());

	// Control: the same request with the challenge the server would have issued does allocate,
	// so the forgeries above were refused for their challenge, not for their layout.
	const SystemAddress honest("10.200.0.1", 5000);
	BitStream request;
	WriteRequest2(request, server->GetHandshakeChallengeForTesting().Generate(net.Now(), honest), info,
	              net.AddressOf(server), 1200, RakNetGUID(0xABCDEFull));
	net.InjectDatagram(honest, server, request.GetData(), (int) request.GetNumberOfBytesUsed());
	net.Step(10);
	EXPECT_NE(IS_NOT_CONNECTED, server->GetConnectionState(honest));
}

TEST(EncryptedHandshake, DuplicateReply2AfterKeysInstalledIsIgnored)
{
	SimulatedNetwork net;
	Capture capture;
	capture.Attach(net);
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const SystemAddress clientAddress = net.AddressOf(client);
	const SystemAddress serverAddress = net.AddressOf(server);

	std::vector<char> reply2;
	for (const Capture::Datagram &d : capture.datagrams)
		if (d.from == serverAddress && !d.bytes.empty() && (unsigned char) d.bytes[0] == ID_OPEN_CONNECTION_REPLY_2)
			reply2 = d.bytes;
	ASSERT_FALSE(reply2.empty());

	SessionKeys before;
	ASSERT_TRUE(KeysOf(client, serverAddress, &before));
	for (int i = 0; i < 3; ++i)
		net.InjectDatagram(serverAddress, client, reinterpret_cast<const unsigned char *>(reply2.data()), (int) reply2.size());
	net.Step(50);

	SessionKeys after;
	ASSERT_TRUE(KeysOf(client, serverAddress, &after));
	EXPECT_EQ(0, std::memcmp(before.send, after.send, sizeof(before.send)));
	EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(serverAddress));

	SendNumbered(client, serverAddress, 5);
	SendNumbered(server, clientAddress, 6);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return NumbersReceived(net, server).size() == 1 && NumbersReceived(net, client).size() == 1; }, 2000));
	EXPECT_EQ(0u, server->GetStatistics(clientAddress)->droppedUnauthenticatedDatagrams);
}

TEST(EncryptedHandshake, ReconnectFromSameAddressUsesFreshKeys)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(2, 2);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));
	const SystemAddress clientAddress = net.AddressOf(client);
	server->SetTimeoutTime(2000, clientAddress);

	SessionKeys first;
	ASSERT_TRUE(KeysOf(server, clientAddress, &first));

	client = net.RestartPeer(client, 1);
	ASSERT_EQ(clientAddress, net.AddressOf(client));
	ASSERT_TRUE(net.RunUntilReceived(server, ID_CONNECTION_LOST, 10000));

	ASSERT_TRUE(net.Connect(client, server, 5000));
	SessionKeys second;
	ASSERT_TRUE(KeysOf(server, clientAddress, &second));
	EXPECT_NE(0, std::memcmp(first.send, second.send, sizeof(first.send)));
	EXPECT_NE(0, std::memcmp(first.recv, second.recv, sizeof(first.recv)));

	SendNumbered(client, net.AddressOf(server), 9);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return NumbersReceived(net, server) == std::vector<uint32_t>{9}; }, 2000));
}

// A non-null PublicKey* pins the server identity (ServerIdentityTests covers the checks); it is no
// longer refused with INVALID_PARAMETER.
TEST(EncryptedHandshake, ConnectWithPublicKeyStartsAPinnedAttempt)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress target = net.AddressOf(server);
	char host[64];
	target.ToString(false, host, sizeof(host));

	PublicKey pin = {};
	EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, client->Connect(host, target.GetPort(), nullptr, 0, &pin));
	// A null socket is still an invalid parameter, pin or not.
	EXPECT_EQ(INVALID_PARAMETER, client->ConnectWithSocket(host, target.GetPort(), nullptr, 0, nullptr, &pin));
	EXPECT_EQ(IS_PENDING, client->GetConnectionState(target));
}

TEST(EncryptedHandshake, SetEncryptionModeAfterStartupFails)
{
	SimulatedNetwork net;
	RakPeer *started = net.AddPeer(1);
	EXPECT_FALSE(started->SetEncryptionMode(EncryptionMode::Plaintext));
	EXPECT_EQ(EncryptionMode::Required, started->GetEncryptionMode());

	RakPeerInterface *idle = RakPeerInterface::GetInstance();
	EXPECT_EQ(EncryptionMode::Required, idle->GetEncryptionMode());
	EXPECT_TRUE(idle->SetEncryptionMode(EncryptionMode::Plaintext));
	EXPECT_EQ(EncryptionMode::Plaintext, idle->GetEncryptionMode());
	EXPECT_TRUE(idle->SetEncryptionMode(EncryptionMode::Required));
	EXPECT_EQ(EncryptionMode::Required, idle->GetEncryptionMode());
	RakPeerInterface::DestroyInstance(idle);
}

TEST(EncryptedHandshake, MtuDiscoveryConvergesWhenEncrypted)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	SimulatedNetwork::LinkRule narrow;
	narrow.maxDatagramBytes = 1200;
	net.SetLinkBothWays(client, server, narrow);
	ASSERT_TRUE(net.Connect(client, server, 20000));

	const int clientSide = client->GetMTUSize(net.AddressOf(server));
	EXPECT_EQ(clientSide, server->GetMTUSize(net.AddressOf(client)));
	EXPECT_LE(clientSide - UDP_HEADER_SIZE, 1200);

	const uint64_t droppedAfterConnect = net.DatagramsDropped();
	BitStream bs;
	bs.Write(ID_TEST_PAYLOAD);
	for (int i = 0; i < 10 * 1024; ++i)
		bs.Write((unsigned char) (i * 31));
	ASSERT_NE(0u, client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, net.AddressOf(server), false));
	ASSERT_TRUE(net.RunUntilReceived(server, ID_TEST_PAYLOAD, 10000));
	const auto *message = net.Find(server, ID_TEST_PAYLOAD);
	ASSERT_EQ((size_t) 1 + 10 * 1024, message->data.size());
	for (int i = 0; i < 10 * 1024; ++i)
		ASSERT_EQ((unsigned char) (i * 31), message->data[1 + i]) << "byte " << i;
	EXPECT_EQ(droppedAfterConnect, net.DatagramsDropped()) << "a datagram exceeded the path MTU";
}

// A lost ID_OPEN_CONNECTION_REPLY_2 makes the client retry; its next REQUEST_2 carries the same crypt
// info, and the server, which already derived its keys, must answer with the same crypt info it sent
// the first time -- not a new key pair the client's keys would not match.
TEST(EncryptedHandshake, LostReply2IsAnsweredWithTheSameServerCryptInfo)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);
	const SystemAddress clientAddress = net.AddressOf(client);

	std::vector<std::vector<char>> replies;
	bool droppedOne = false;
	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *data, int length)
	           {
		           if (from != serverAddress)
			           return;
		           const bool isReply2 = length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_2;
		           if (isReply2)
			           replies.emplace_back(data, data + length);
		           // The tap runs before the link rules: darken the link for the first REPLY_2 only.
		           net.SetLink(serverAddress, clientAddress, isReply2 && !droppedOne ? dark : SimulatedNetwork::LinkRule());
		           droppedOne = droppedOne || isReply2; });

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED) && net.Received(server, ID_NEW_INCOMING_CONNECTION); }, 10000));
	ASSERT_GE(replies.size(), 2u);
	// The server crypt info and the identity block (1 byte, absent: this server has no identity key) end
	// REPLY_2 (the MTU before them may differ: the retry can probe a lower rung).
	const size_t tail = (size_t) CryptInfo::kSerializedBytes + IdentityBlock::kAbsentBytes;
	ASSERT_GT(replies[0].size(), tail);
	ASSERT_EQ(replies[0].size(), replies[1].size());
	EXPECT_EQ(0, replies[0].back()) << "identity flag";
	EXPECT_TRUE(std::equal(replies[0].end() - (long) tail, replies[0].end(), replies[1].end() - (long) tail))
	    << "the retransmitted REPLY_2 carries a different server crypt info";

	SessionKeys clientKeys, serverKeys;
	ASSERT_TRUE(KeysOf(client, serverAddress, &clientKeys));
	ASSERT_TRUE(KeysOf(server, clientAddress, &serverKeys));
	EXPECT_EQ(0, std::memcmp(clientKeys.send, serverKeys.recv, sizeof(clientKeys.send)));
}

namespace
{
// ID_OPEN_CONNECTION_REPLY_2, as a server without an identity key writes it:
// id | offline id (16) | nonce echo (8) | server guid (8) | client address | mtu (2) | server crypt info (41).
void WriteReply2(BitStream &out, uint64_t nonceEcho, const RakNetGUID &serverGuid, const SystemAddress &clientAddress,
                 uint16_t mtu, const CryptInfo &serverInfo)
{
	out.Write((MessageID) ID_OPEN_CONNECTION_REPLY_2);
	out.WriteAlignedBytes(kOfflineMessageDataId, kOfflineMessageDataIdSize);
	out.Write(nonceEcho);
	out.Write(serverGuid);
	out.Write(clientAddress);
	out.Write(mtu);
	serverInfo.Serialize(&out);
	IdentityBlock().Serialize(&out); // no identity key
}

// A connection attempt from a fresh client to an address no peer owns, so the only REPLY_2 it can get is
// one the test injects. Returns the client; *target is the address it is connecting to.
RakPeer *StartAttemptToNowhere(SimulatedNetwork &net, SystemAddress *target)
{
	RakPeer *client = net.AddPeer(1);
	*target = SystemAddress("10.0.0.250", 5000);
	EXPECT_EQ(CONNECTION_ATTEMPT_STARTED, client->Connect("10.0.0.250", 5000, nullptr, 0));
	net.Step(10);
	return client;
}
} // namespace

// REPLY_2 echoes the nonce of the client's crypt info, and the client drops a REPLY_2 whose echo does not
// match its pending attempt before anything else: no key derivation, no connection, no failure reported.
TEST(EncryptedHandshake, ForgedReply2WithTheWrongNonceEchoIsIgnored)
{
	SimulatedNetwork net;
	SystemAddress target;
	RakPeer *client = StartAttemptToNowhere(net, &target);
	const SystemAddress clientAddress = net.AddressOf(client);
	uint64_t nonce = 0;
	ASSERT_TRUE(client->GetPendingHandshakeNonceForTesting(target, &nonce));

	EphemeralKeyPair serverPair;
	const CryptInfo serverInfo = serverPair.MakeCryptInfo();
	CryptInfo plaintextInfo;
	plaintextInfo.mode = EncryptionMode::Plaintext;
	const uint64_t wrongEchoes[] = {nonce ^ 1, nonce + ((uint64_t) 1 << 32), ~nonce, 0};
	for (uint64_t wrong : wrongEchoes)
	{
		if (wrong == nonce)
			continue;
		BitStream reply2;
		WriteReply2(reply2, wrong, RakNetGUID(0x5E5E5E5Eull), clientAddress, 1200, serverInfo);
		net.InjectDatagram(target, client, reply2.GetData(), (int) reply2.GetNumberOfBytesUsed());
		// A mode mismatch would fail the attempt -- but only once the echo matches.
		BitStream mismatch;
		WriteReply2(mismatch, wrong, RakNetGUID(0x5E5E5E5Eull), clientAddress, 1200, plaintextInfo);
		net.InjectDatagram(target, client, mismatch.GetData(), (int) mismatch.GetNumberOfBytesUsed());
	}
	net.Step(20);

	SessionKeys keys;
	EXPECT_FALSE(KeysOf(client, target, &keys)) << "keys were derived from a forged REPLY_2";
	EXPECT_EQ(IS_PENDING, client->GetConnectionState(target));
	EXPECT_TRUE(net.Inbox(client).empty()) << "a forged REPLY_2 produced a message";

	// Control: the same REPLY_2 with the right echo is taken as the answer to the attempt.
	BitStream reply2;
	WriteReply2(reply2, nonce, RakNetGUID(0x5E5E5E5Eull), clientAddress, 1200, serverInfo);
	net.InjectDatagram(target, client, reply2.GetData(), (int) reply2.GetNumberOfBytesUsed());
	net.Step(10);
	EXPECT_TRUE(KeysOf(client, target, &keys));
}

// In mode Plaintext there is no ephemeral key, but the client's crypt info still carries a random nonce,
// so the echo protects a Plaintext attempt too. The public key stays zero.
TEST(EncryptedHandshake, PlaintextClientCryptInfoCarriesARandomNonceThatReply2Echoes)
{
	SimulatedNetwork net;
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);
	std::vector<std::vector<char>> requests, replies;
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *data, int length)
	           {
		           if (length <= 0)
			           return;
		           if ((unsigned char) data[0] == ID_OPEN_CONNECTION_REQUEST_2)
			           requests.emplace_back(data, data + length);
		           else if (from == serverAddress && (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_2)
			           replies.emplace_back(data, data + length); });

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(client, server));
	uint64_t nonce = 0;
	ASSERT_TRUE(client->GetPendingHandshakeNonceForTesting(serverAddress, &nonce));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED); }, 5000));
	ASSERT_FALSE(requests.empty());
	ASSERT_FALSE(replies.empty());

	BitStream request(reinterpret_cast<unsigned char *>(requests[0].data()), (unsigned int) requests[0].size(), false);
	request.IgnoreBytes(1 + kOfflineMessageDataIdSize + sizeof(uint64_t));
	CryptInfo clientInfo;
	ASSERT_TRUE(clientInfo.Deserialize(&request));
	EXPECT_EQ(EncryptionMode::Plaintext, clientInfo.mode);
	const uint8_t zero[32] = {};
	EXPECT_EQ(0, std::memcmp(zero, clientInfo.publicKey, sizeof(zero)));
	EXPECT_EQ(nonce, clientInfo.nonce);
	EXPECT_NE(0u, clientInfo.nonce);

	BitStream reply(reinterpret_cast<unsigned char *>(replies[0].data()), (unsigned int) replies[0].size(), false);
	reply.IgnoreBytes(1 + kOfflineMessageDataIdSize);
	uint64_t echo = 0;
	ASSERT_TRUE(reply.Read(echo));
	EXPECT_EQ(nonce, echo);

	// A second attempt draws a different nonce.
	RakPeer *other = net.AddPeer(1);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, net.StartConnect(other, server));
	uint64_t otherNonce = 0;
	ASSERT_TRUE(other->GetPendingHandshakeNonceForTesting(serverAddress, &otherNonce));
	EXPECT_NE(nonce, otherNonce);
}

// REQUEST_1 shorter than the REPLY_1 it would trigger (35 bytes) is ignored, so the server never answers
// with more than it received.
TEST(EncryptedHandshake, Request1ShorterThanReply1IsIgnored)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(4, 4);
	const SystemAddress serverAddress = net.AddressOf(server);
	std::vector<int> reply1Sizes;
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *data, int length)
	           {
		           if (from == serverAddress && length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REPLY_1)
			           reply1Sizes.push_back(length); });

	auto request1 = [](int length)
	{
		std::vector<unsigned char> bytes((size_t) length, 0);
		bytes[0] = ID_OPEN_CONNECTION_REQUEST_1;
		std::memcpy(&bytes[1], kOfflineMessageDataId, kOfflineMessageDataIdSize);
		bytes[1 + kOfflineMessageDataIdSize] = RAKNET_PROTOCOL_VERSION;
		return bytes;
	};

	const SystemAddress sender("10.0.0.77", 7000);
	for (int length = 1 + (int) kOfflineMessageDataIdSize + 1; length < 35; ++length)
	{
		const std::vector<unsigned char> bytes = request1(length);
		net.InjectDatagram(sender, server, bytes.data(), length);
	}
	net.Step(20);
	EXPECT_TRUE(reply1Sizes.empty()) << "a REQUEST_1 shorter than REPLY_1 was answered";

	const std::vector<unsigned char> minimal = request1(35);
	net.InjectDatagram(sender, server, minimal.data(), 35);
	net.Step(20);
	ASSERT_EQ(1u, reply1Sizes.size());
	EXPECT_EQ(35, reply1Sizes[0]) << "REPLY_1 changed size: update the REQUEST_1 minimum with it";
}

// The smallest MTU probe a client sends (the bottom rung of the ladder) still clears the REQUEST_1 minimum.
TEST(EncryptedHandshake, SmallestMtuProbeStillConnects)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	SimulatedNetwork::LinkRule narrow;
	narrow.maxDatagramBytes = MTU_LADDER[MTU_LADDER_SIZE - 1] - UDP_HEADER_SIZE;
	net.SetLinkBothWays(client, server, narrow);
	ASSERT_TRUE(net.Connect(client, server, 30000));
	EXPECT_EQ(MTU_LADDER[MTU_LADDER_SIZE - 1], client->GetMTUSize(net.AddressOf(server)));
}

// With SetLimitIPConnectionFrequency, a second REQUEST_2 from an IP that connected less than 100 ms ago is
// refused with ID_IP_RECENTLY_CONNECTED before the server runs its X25519.
TEST(EncryptedHandshake, ConnectedRecentlyRefusalComesBeforeTheKeyExchange)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	server->SetLimitIPConnectionFrequency(true);
	const SystemAddress serverAddress = net.AddressOf(server);
	size_t recentlyConnected = 0;
	net.SetTap([&](const SystemAddress &from, const SystemAddress &, const char *data, int length)
	           { recentlyConnected += from == serverAddress && length > 0 && (unsigned char) data[0] == ID_IP_RECENTLY_CONNECTED; });

	auto inject = [&](const SystemAddress &from, uint64_t guid)
	{
		EphemeralKeyPair pair;
		BitStream request;
		WriteRequest2(request, server->GetHandshakeChallengeForTesting().Generate(net.Now(), from), pair.MakeCryptInfo(),
		              serverAddress, 1200, RakNetGUID(guid));
		net.InjectDatagram(from, server, request.GetData(), (int) request.GetNumberOfBytesUsed());
	};

	const SystemAddress first("10.0.0.90", 6000);
	inject(first, 0xA1);
	net.Step(5);
	ASSERT_NE(IS_NOT_CONNECTED, server->GetConnectionState(first));
	EXPECT_EQ(1u, server->GetServerKeyExchangeCountForTesting());

	const SystemAddress samePc("10.0.0.90", 6001);
	inject(samePc, 0xA2);
	net.Step(5);
	EXPECT_EQ(1u, recentlyConnected);
	EXPECT_EQ(IS_NOT_CONNECTED, server->GetConnectionState(samePc));
	EXPECT_EQ(1u, server->GetServerKeyExchangeCountForTesting()) << "X25519 ran for a refused REQUEST_2";

	// Same semantics as before: after 100 ms the same IP is answered (and keyed) again.
	net.Step(200);
	const SystemAddress later("10.0.0.90", 6002);
	inject(later, 0xA3);
	net.Step(5);
	EXPECT_NE(IS_NOT_CONNECTED, server->GetConnectionState(later));
	EXPECT_EQ(2u, server->GetServerKeyExchangeCountForTesting());
	EXPECT_EQ(1u, recentlyConnected);
}
