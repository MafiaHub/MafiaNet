/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/Dispatcher.h"
#include "mafianet/PeerHandle.h" // Peer, PacketPtr, send()/broadcast()
#include "mafianet/GetTime.h"    // GetTimeMS
#include "mafianet/sleep.h"      // RakSleep

#include <string>

using namespace MafiaNet;

namespace
{
	// A distinct port so a full-suite run does not collide with other tests.
	const unsigned short TYPED_SEND_TEST_PORT = 61018;

	// One typed message, registered identically on both ends so encode()/dispatch()
	// agree on its auto-assigned id (ID_USER_PACKET_ENUM).
	struct ChatMessage
	{
		std::string text;
		int channel = 0;

		template <class Ar>
		void serialize(Ar &ar) { ar & text & channel; }

		bool operator==(const ChatMessage &o) const { return text == o.text && channel == o.channel; }
	};

	// Pump both peers once, running server-received packets through serverD and
	// client-received packets through clientD.
	void Pump(Peer &server, Dispatcher &serverD, Peer &client, Dispatcher &clientD)
	{
		for (auto pkt : server.incoming())
			serverD.dispatch(pkt);
		for (auto pkt : client.incoming())
			clientD.dispatch(pkt);
	}
}

class TypedSendLive : public ::testing::Test
{
	// Peers in this test are RAII MafiaNet::Peer locals in the test body; they
	// shut down and destroy themselves even when an ASSERT bails out early.
};

TEST_F(TypedSendLive, SendDeliversTypedMessagesAndBroadcastReachesAllPeers)
{
	// One server, two clients: the two clients let broadcast() be checked against
	// "reaches all connected peers", not just one. Space out the GetInstance()
	// calls (each Peer constructs one): on POSIX the GUID is seeded from the
	// microsecond clock, so back-to-back peers would otherwise get identical GUIDs
	// and the server would drop the duplicate connection.
	Peer server;
	RakSleep(2);
	Peer clientA;
	RakSleep(2);
	Peer clientB;

	SocketDescriptor serverSd(TYPED_SEND_TEST_PORT, 0);
	ASSERT_EQ(server->Startup(2, &serverSd, 1), RAKNET_STARTED)
		<< "Server Startup failed (port " << TYPED_SEND_TEST_PORT << " in use?)";
	server->SetMaximumIncomingConnections(2);

	SocketDescriptor clientSd;
	ASSERT_TRUE(clientA->Startup(1, &clientSd, 1) == RAKNET_STARTED && clientB->Startup(1, &clientSd, 1) == RAKNET_STARTED)
		<< "Client Startup failed";

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(TYPED_SEND_TEST_PORT);

	// Server dispatcher: counts typed ChatMessages and remembers the last one.
	int chatReceived = 0;
	ChatMessage lastChat;
	RakNetGUID lastChatGuid;
	Dispatcher serverD;
	serverD.on<ChatMessage>([&](const ChatMessage &m, const Sender &from) {
		++chatReceived;
		lastChat = m;
		lastChatGuid = from.guid();
	});

	// Each client dispatcher registers ChatMessage in the same order -> same id.
	int chatA = 0, chatB = 0;
	ChatMessage lastA, lastB;
	Dispatcher clientAD, clientBD;
	clientAD.on<ChatMessage>([&](const ChatMessage &m, const Sender &) { ++chatA; lastA = m; });
	clientBD.on<ChatMessage>([&](const ChatMessage &m, const Sender &) { ++chatB; lastB = m; });

	ASSERT_TRUE(clientA->Connect("127.0.0.1", TYPED_SEND_TEST_PORT, 0, 0) == CONNECTION_ATTEMPT_STARTED &&
	            clientB->Connect("127.0.0.1", TYPED_SEND_TEST_PORT, 0, 0) == CONNECTION_ATTEMPT_STARTED)
		<< "Connect did not start";

	// Wait until the server reports two connected systems.
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && server->NumberOfConnections() < 2)
	{
		for (auto pkt : server.incoming()) (void) pkt;
		for (auto pkt : clientA.incoming()) (void) pkt;
		for (auto pkt : clientB.incoming()) (void) pkt;
		RakSleep(30);
	}
	ASSERT_GE(server->NumberOfConnections(), 2u) << "Both clients did not connect within 10s";

	// --- 1. Unicast with defaults: clientA -> server ---
	ChatMessage m1;
	m1.text = std::string("a\0b default send", 16); // binary-safe payload
	m1.channel = 7;
	uint32_t receipt = clientA.send(clientAD, m1, serverAddress);
	ASSERT_NE(receipt, 0u) << "Default send returned a 0 receipt (bad input)";

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && chatReceived < 1)
	{
		Pump(server, serverD, clientA, clientAD);
		for (auto pkt : clientB.incoming()) clientBD.dispatch(pkt);
		RakSleep(30);
	}
	ASSERT_TRUE(chatReceived >= 1 && lastChat == m1)
		<< "Default send did not round-trip (received=" << chatReceived << ")";
	ASSERT_EQ(lastChatGuid, clientA->GetMyGUID()) << "Sender guid mismatch on default send";

	// --- 2. Reliability overridable per call: clientA -> server, ReliableSequenced ---
	ChatMessage m2;
	m2.text = "explicit reliability";
	m2.channel = -1;
	ASSERT_NE(clientA.send(clientAD, m2, serverAddress, Reliability::ReliableSequenced), 0u)
		<< "Explicit-reliability send returned a 0 receipt";

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && chatReceived < 2)
	{
		Pump(server, serverD, clientA, clientAD);
		for (auto pkt : clientB.incoming()) clientBD.dispatch(pkt);
		RakSleep(30);
	}
	ASSERT_TRUE(chatReceived >= 2 && lastChat == m2)
		<< "Explicit-reliability send did not round-trip (received=" << chatReceived << ")";

	// --- 3. Broadcast reaches every connected peer: server -> {clientA, clientB} ---
	ChatMessage bc;
	bc.text = "broadcast to all";
	bc.channel = 42;
	ASSERT_NE(server.broadcast(serverD, bc), 0u) << "Broadcast returned a 0 receipt";

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && (chatA < 1 || chatB < 1))
	{
		for (auto pkt : server.incoming()) serverD.dispatch(pkt);
		for (auto pkt : clientA.incoming()) clientAD.dispatch(pkt);
		for (auto pkt : clientB.incoming()) clientBD.dispatch(pkt);
		RakSleep(30);
	}
	ASSERT_TRUE(chatA >= 1 && chatB >= 1)
		<< "Broadcast did not reach both clients (A=" << chatA << " B=" << chatB << ")";
	ASSERT_TRUE(lastA == bc && lastB == bc) << "Broadcast payload mismatch";
}
