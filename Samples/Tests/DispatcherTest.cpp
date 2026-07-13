/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "DispatcherTest.h"

#include <string>

#include "mafianet/PeerHandle.h" // Peer, PacketPtr (live round-trip)
#include "mafianet/GetTime.h"    // GetTime, GetTimeMS
#include "mafianet/sleep.h"      // RakSleep

namespace
{
	// A distinct port so a full-suite run does not collide with other tests.
	const unsigned short DISPATCHER_TEST_PORT = 61016;

	// Two typed messages. Registration order (ChatMessage then PlayerMove) fixes
	// their auto-assigned ids at ID_USER_PACKET_ENUM and ID_USER_PACKET_ENUM+1.
	struct ChatMessage
	{
		std::string text;
		int channel = 0;

		template <class Ar>
		void serialize(Ar &ar) { ar & text & channel; }

		bool operator==(const ChatMessage &o) const { return text == o.text && channel == o.channel; }
	};

	struct PlayerMove
	{
		float x = 0.f, y = 0.f;
		unsigned int tick = 0;

		template <class Ar>
		void serialize(Ar &ar) { ar & x & y & tick; }

		bool operator==(const PlayerMove &o) const { return x == o.x && y == o.y && tick == o.tick; }
	};

	// Synthesize a received Packet that borrows the bytes written into a BitStream,
	// so dispatch() can be exercised without a live connection.
	Packet MakePacket(BitStream &bs, const RakNetGUID &guid)
	{
		Packet p{};
		p.data = bs.GetData();
		p.length = bs.GetNumberOfBytesUsed();
		p.bitSize = bs.GetNumberOfBitsUsed();
		p.guid = guid;
		return p;
	}
}

int DispatcherTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	// --- 1. Auto-assigned id contract: registration order fixes the ids ---
	{
		Dispatcher d;
		MessageID chatId = d.on<ChatMessage>([](const ChatMessage &, const Sender &) {});
		MessageID moveId = d.on<PlayerMove>([](const PlayerMove &, const Sender &) {});

		if (chatId != (MessageID) ID_USER_PACKET_ENUM || moveId != (MessageID) (ID_USER_PACKET_ENUM + 1))
		{
			if (isVerbose) printf("auto ids wrong: chat=%u move=%u\n", chatId, moveId);
			return 1;
		}
		if (!d.has<ChatMessage>() || !d.has<PlayerMove>())
		{
			if (isVerbose) printf("has<T>() false for a registered type\n");
			return 2;
		}
		if (d.id_of<ChatMessage>() != chatId || d.id_of<PlayerMove>() != moveId)
		{
			if (isVerbose) printf("id_of<>() disagrees with assigned ids\n");
			return 2;
		}
	}

	// --- 2. Offline round-trip via encode()/dispatch(), no network ---
	{
		Dispatcher d;
		d.on<ChatMessage>([](const ChatMessage &, const Sender &) {});

		ChatMessage sent;
		sent.text = std::string("a\0b hidden null", 15); // binary-safe payload
		sent.channel = -42;

		ChatMessage got;
		bool fired = false;

		Dispatcher r;
		r.on<ChatMessage>([&](const ChatMessage &m, const Sender &from) {
			got = m;
			fired = true;
			(void) from;
		});

		BitStream bs;
		d.encode(bs, sent);

		RakNetGUID sender(0x1234567890abcdefULL);
		Packet packet = MakePacket(bs, sender);
		if (!r.dispatch(packet))
		{
			if (isVerbose) printf("dispatch() returned false for a registered id\n");
			return 3;
		}
		if (!fired || !(got == sent))
		{
			if (isVerbose) printf("offline round-trip mismatch: text='%s' channel=%d\n", got.text.c_str(), got.channel);
			return 4;
		}
	}

	// --- 3. ID_TIMESTAMP-prefixed packet dispatches to the correct handler ---
	{
		Dispatcher d;
		d.on<PlayerMove>([](const PlayerMove &, const Sender &) {});

		PlayerMove sent;
		sent.x = 3.5f; sent.y = -1.25f; sent.tick = 99;

		PlayerMove got;
		bool fired = false;
		Dispatcher r;
		// Same registration order -> same id as the encoder above.
		r.on<PlayerMove>([&](const PlayerMove &m, const Sender &) { got = m; fired = true; });

		// Wire: ID_TIMESTAMP, Time, then the encoded typed message (id + body).
		BitStream bs;
		bs.Write((MessageID) ID_TIMESTAMP);
		bs.Write(GetTime());
		d.encode(bs, sent);

		Packet packet = MakePacket(bs, UNASSIGNED_RAKNET_GUID);
		if (!r.dispatch(packet) || !fired || !(got == sent))
		{
			if (isVerbose) printf("timestamp-prefixed dispatch failed (fired=%d)\n", (int) fired);
			return 5;
		}
	}

	// --- 4. Unregistered id and truncated packets leave the switch path usable ---
	{
		Dispatcher d; // nothing registered
		BitStream bs;
		bs.Write((MessageID) (ID_USER_PACKET_ENUM + 50));
		Packet packet = MakePacket(bs, UNASSIGNED_RAKNET_GUID);
		if (d.dispatch(packet))
		{
			if (isVerbose) printf("dispatch() claimed to handle an unregistered id\n");
			return 6;
		}

		// A lone ID_TIMESTAMP with no room for the Time prefix must not read OOB.
		BitStream truncated;
		truncated.Write((MessageID) ID_TIMESTAMP);
		Packet tpacket = MakePacket(truncated, UNASSIGNED_RAKNET_GUID);
		if (d.dispatch(tpacket))
		{
			if (isVerbose) printf("dispatch() handled a truncated timestamp packet\n");
			return 7;
		}
	}

	// --- 5. Explicit-id registration bypasses registration-order coupling ---
	{
		Dispatcher a, b;
		const MessageID kExplicit = (MessageID) (ID_USER_PACKET_ENUM + 30);
		// a registers a decoy first, so its *auto* id would differ from b's.
		a.on<PlayerMove>([](const PlayerMove &, const Sender &) {});
		a.on<ChatMessage>(kExplicit, [](const ChatMessage &, const Sender &) {});

		bool fired = false;
		ChatMessage got;
		b.on<ChatMessage>(kExplicit, [&](const ChatMessage &m, const Sender &) { got = m; fired = true; });

		ChatMessage sent; sent.text = "explicit"; sent.channel = 5;
		BitStream bs;
		a.encode(bs, sent); // uses kExplicit despite different registration order
		Packet packet = MakePacket(bs, UNASSIGNED_RAKNET_GUID);
		if (!b.dispatch(packet) || !fired || !(got == sent))
		{
			if (isVerbose) printf("explicit-id round-trip failed\n");
			return 8;
		}
	}

	// --- 6. Live loopback: real send, system-id handler, and Sender identity ---
	Peer server;
	Peer client;

	SocketDescriptor serverSd(DISPATCHER_TEST_PORT, 0);
	if (server->Startup(1, &serverSd, 1) != RAKNET_STARTED)
	{
		if (isVerbose) printf("server Startup failed (port %u in use?)\n", DISPATCHER_TEST_PORT);
		return 20;
	}
	server->SetMaximumIncomingConnections(1);

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{
		if (isVerbose) printf("client Startup failed\n");
		return 21;
	}

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(DISPATCHER_TEST_PORT);

	// Server dispatcher: a system-id handler and a typed handler.
	bool sawIncomingConnection = false;
	SystemAddress incomingFrom;
	int chatReceived = 0;
	ChatMessage lastChat;
	RakNetGUID lastChatGuid;

	Dispatcher serverD;
	serverD.on(ID_NEW_INCOMING_CONNECTION, [&](const Sender &s) {
		sawIncomingConnection = true;
		incomingFrom = s.address();
	});
	MessageID serverChatId = serverD.on<ChatMessage>([&](const ChatMessage &m, const Sender &from) {
		++chatReceived;
		lastChat = m;
		lastChatGuid = from.guid();
	});

	// Client dispatcher: registers ChatMessage identically, so encode() picks the
	// same id the server dispatches on (the registration-order contract).
	Dispatcher clientD;
	MessageID clientChatId = clientD.on<ChatMessage>([](const ChatMessage &, const Sender &) {});
	if (serverChatId != clientChatId)
	{
		if (isVerbose) printf("client/server disagree on ChatMessage id (%u vs %u)\n", serverChatId, clientChatId);
		return 22;
	}

	if (client->Connect("127.0.0.1", DISPATCHER_TEST_PORT, 0, 0) != CONNECTION_ATTEMPT_STARTED)
	{
		if (isVerbose) printf("Connect did not start\n");
		return 23;
	}

	// Wait for the connection to be accepted on the client, draining the server
	// through the dispatcher so its ID_NEW_INCOMING_CONNECTION handler fires.
	bool connected = false;
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !connected)
	{
		for (auto pkt : server.incoming())
			serverD.dispatch(pkt);
		for (auto pkt : client.incoming())
		{
			if (pkt.id() == ID_CONNECTION_REQUEST_ACCEPTED)
				connected = true;
		}
		RakSleep(30);
	}
	if (!connected)
	{
		if (isVerbose) printf("client did not connect within 10s\n");
		return 24;
	}
	if (!sawIncomingConnection)
	{
		if (isVerbose) printf("system-id handler (ID_NEW_INCOMING_CONNECTION) never fired\n");
		return 25;
	}

	// Send a plain typed ChatMessage from client to server and dispatch it.
	ChatMessage outgoing;
	outgoing.text = "hello from client";
	outgoing.channel = 7;
	{
		BitStream bs;
		clientD.encode(bs, outgoing);
		client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false);
	}

	// Send a second ChatMessage behind an ID_TIMESTAMP prefix.
	ChatMessage outgoingTs;
	outgoingTs.text = "hello with timestamp";
	outgoingTs.channel = -3;
	{
		BitStream bs;
		bs.Write((MessageID) ID_TIMESTAMP);
		bs.Write(GetTime());
		clientD.encode(bs, outgoingTs);
		client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false);
	}

	RakNetGUID clientGuid = client->GetMyGUID();

	start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && chatReceived < 2)
	{
		for (auto pkt : server.incoming())
			serverD.dispatch(pkt);
		for (auto pkt : client.incoming())
			(void) pkt;
		RakSleep(30);
	}
	if (chatReceived < 2)
	{
		if (isVerbose) printf("server dispatched %d of 2 ChatMessages\n", chatReceived);
		return 26;
	}
	// The last message received was the timestamp-prefixed one; its fields must match.
	if (!(lastChat == outgoingTs))
	{
		if (isVerbose) printf("timestamp ChatMessage fields mismatch: '%s'/%d\n", lastChat.text.c_str(), lastChat.channel);
		return 27;
	}
	// Sender identity: the handler saw the client's GUID.
	if (lastChatGuid != clientGuid)
	{
		if (isVerbose) printf("Sender.guid() did not match the client GUID\n");
		return 28;
	}

	printf("Dispatcher routes typed, timestamped, and system messages correctly\n");
	return 0;
}

DispatcherTest::DispatcherTest(void)
{
}

DispatcherTest::~DispatcherTest(void)
{
}

RakString DispatcherTest::GetTestName(void)
{
	return "DispatcherTest";
}

RakString DispatcherTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "Auto-assigned ids did not follow registration order";
	case 2:  return "id_of<>() disagreed with assigned ids";
	case 3:  return "dispatch() returned false for a registered id";
	case 4:  return "Offline typed round-trip mismatch";
	case 5:  return "ID_TIMESTAMP-prefixed dispatch failed";
	case 6:  return "dispatch() handled an unregistered id";
	case 7:  return "dispatch() handled a truncated timestamp packet";
	case 8:  return "Explicit-id round-trip failed";
	case 20: return "Server Startup failed";
	case 21: return "Client Startup failed";
	case 22: return "Client/server disagree on the auto-assigned id";
	case 23: return "Connect did not start";
	case 24: return "Client did not connect";
	case 25: return "System-id handler never fired";
	case 26: return "Server did not dispatch both ChatMessages";
	case 27: return "Live ChatMessage fields mismatch";
	case 28: return "Sender.guid() mismatch";
	default: return "Undefined error";
	}
}

void DispatcherTest::DestroyPeers(void)
{
	// Peers in this test are RAII MafiaNet::Peer locals; nothing to do here.
}
