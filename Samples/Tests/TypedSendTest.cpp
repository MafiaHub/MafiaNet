/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "TypedSendTest.h"

#include <string>

#include "mafianet/PeerHandle.h" // Peer, PacketPtr, send()/broadcast()
#include "mafianet/GetTime.h"    // GetTimeMS
#include "mafianet/sleep.h"      // RakSleep

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

int TypedSendTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

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
	if (server->Startup(2, &serverSd, 1) != RAKNET_STARTED)
	{
		if (isVerbose) printf("server Startup failed (port %u in use?)\n", TYPED_SEND_TEST_PORT);
		return 1;
	}
	server->SetMaximumIncomingConnections(2);

	SocketDescriptor clientSd;
	if (clientA->Startup(1, &clientSd, 1) != RAKNET_STARTED || clientB->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{
		if (isVerbose) printf("client Startup failed\n");
		return 2;
	}

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

	if (clientA->Connect("127.0.0.1", TYPED_SEND_TEST_PORT, 0, 0) != CONNECTION_ATTEMPT_STARTED ||
	    clientB->Connect("127.0.0.1", TYPED_SEND_TEST_PORT, 0, 0) != CONNECTION_ATTEMPT_STARTED)
	{
		if (isVerbose) printf("Connect did not start\n");
		return 3;
	}

	// Wait until the server reports two connected systems.
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && server->NumberOfConnections() < 2)
	{
		for (auto pkt : server.incoming()) (void) pkt;
		for (auto pkt : clientA.incoming()) (void) pkt;
		for (auto pkt : clientB.incoming()) (void) pkt;
		RakSleep(30);
	}
	if (server->NumberOfConnections() < 2)
	{
		if (isVerbose) printf("both clients did not connect within 10s\n");
		return 4;
	}

	// --- 1. Unicast with defaults: clientA -> server ---
	ChatMessage m1;
	m1.text = std::string("a\0b default send", 16); // binary-safe payload
	m1.channel = 7;
	uint32_t receipt = clientA.send(clientAD, m1, serverAddress);
	if (receipt == 0)
	{
		if (isVerbose) printf("send() returned a 0 receipt (bad input)\n");
		return 5;
	}

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && chatReceived < 1)
	{
		Pump(server, serverD, clientA, clientAD);
		for (auto pkt : clientB.incoming()) clientBD.dispatch(pkt);
		RakSleep(30);
	}
	if (chatReceived < 1 || !(lastChat == m1))
	{
		if (isVerbose) printf("default send did not round-trip (received=%d)\n", chatReceived);
		return 6;
	}
	if (lastChatGuid != clientA->GetMyGUID())
	{
		if (isVerbose) printf("Sender guid mismatch on default send\n");
		return 7;
	}

	// --- 2. Reliability overridable per call: clientA -> server, ReliableSequenced ---
	ChatMessage m2;
	m2.text = "explicit reliability";
	m2.channel = -1;
	if (clientA.send(clientAD, m2, serverAddress, Reliability::ReliableSequenced) == 0)
	{
		if (isVerbose) printf("send() with explicit reliability returned a 0 receipt\n");
		return 8;
	}

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && chatReceived < 2)
	{
		Pump(server, serverD, clientA, clientAD);
		for (auto pkt : clientB.incoming()) clientBD.dispatch(pkt);
		RakSleep(30);
	}
	if (chatReceived < 2 || !(lastChat == m2))
	{
		if (isVerbose) printf("explicit-reliability send did not round-trip (received=%d)\n", chatReceived);
		return 9;
	}

	// --- 3. Broadcast reaches every connected peer: server -> {clientA, clientB} ---
	ChatMessage bc;
	bc.text = "broadcast to all";
	bc.channel = 42;
	if (server.broadcast(serverD, bc) == 0)
	{
		if (isVerbose) printf("broadcast() returned a 0 receipt\n");
		return 10;
	}

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && (chatA < 1 || chatB < 1))
	{
		for (auto pkt : server.incoming()) serverD.dispatch(pkt);
		for (auto pkt : clientA.incoming()) clientAD.dispatch(pkt);
		for (auto pkt : clientB.incoming()) clientBD.dispatch(pkt);
		RakSleep(30);
	}
	if (chatA < 1 || chatB < 1)
	{
		if (isVerbose) printf("broadcast did not reach both clients (A=%d B=%d)\n", chatA, chatB);
		return 11;
	}
	if (!(lastA == bc) || !(lastB == bc))
	{
		if (isVerbose) printf("broadcast payload mismatch\n");
		return 12;
	}

	printf("Peer::send delivers typed messages (defaults + overrides); broadcast reaches all peers\n");
	return 0;
}

TypedSendTest::TypedSendTest(void)
{
}

TypedSendTest::~TypedSendTest(void)
{
}

RakString TypedSendTest::GetTestName(void)
{
	return "TypedSendTest";
}

RakString TypedSendTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "Server Startup failed";
	case 2:  return "Client Startup failed";
	case 3:  return "Connect did not start";
	case 4:  return "Both clients did not connect";
	case 5:  return "Default send returned a 0 receipt";
	case 6:  return "Default send did not round-trip";
	case 7:  return "Sender guid mismatch on default send";
	case 8:  return "Explicit-reliability send returned a 0 receipt";
	case 9:  return "Explicit-reliability send did not round-trip";
	case 10: return "Broadcast returned a 0 receipt";
	case 11: return "Broadcast did not reach both clients";
	case 12: return "Broadcast payload mismatch";
	default: return "Undefined error";
	}
}

void TypedSendTest::DestroyPeers(void)
{
	// Peers in this test are RAII MafiaNet::Peer locals; nothing to do here.
}
