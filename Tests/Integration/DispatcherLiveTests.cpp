/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/Dispatcher.h"
#include "mafianet/PeerHandle.h" // Peer, PacketPtr (live round-trip)
#include "mafianet/GetTime.h"    // GetTime, GetTimeMS
#include "mafianet/sleep.h"      // RakSleep

#include <string>

using namespace MafiaNet;

namespace
{
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
}

class DispatcherLive : public ::testing::Test
{
	// Peers in this test are RAII MafiaNet::Peer locals in the test body; they
	// shut down and destroy themselves even when an ASSERT bails out early.
};

// --- Live loopback: real send, system-id handler, and Sender identity ---
TEST_F(DispatcherLive, RoutesTypedTimestampedAndSystemMessages)
{
	Peer server;
	Peer client;

	// An OS-assigned ephemeral port: a fixed port can still be lingering in the
	// kernel from an earlier test or run and intermittently break Startup.
	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(1, &serverSd, 1), RAKNET_STARTED) << "Server Startup failed";
	server->SetMaximumIncomingConnections(1);
	const unsigned short serverPort = server->GetInternalID().GetPort();

	SocketDescriptor clientSd;
	ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED) << "Client Startup failed";

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);

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
	ASSERT_EQ(serverChatId, clientChatId)
		<< "Client/server disagree on the auto-assigned ChatMessage id ("
		<< (unsigned) serverChatId << " vs " << (unsigned) clientChatId << ")";

	ASSERT_EQ(client->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED) << "Connect did not start";

	// Wait for the connection to be accepted on the client, draining the server
	// through the dispatcher so its ID_NEW_INCOMING_CONNECTION handler fires.
	// Keep pumping until BOTH sides have observed the connection: the server's
	// ID_NEW_INCOMING_CONNECTION is enqueued by its network thread asynchronously,
	// so it can surface after the client has already seen the acceptance.
	bool connected = false;
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !(connected && sawIncomingConnection))
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
	ASSERT_TRUE(connected) << "Client did not connect within 10s";
	ASSERT_TRUE(sawIncomingConnection) << "System-id handler (ID_NEW_INCOMING_CONNECTION) never fired";

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
	ASSERT_GE(chatReceived, 2) << "Server did not dispatch both ChatMessages (dispatched " << chatReceived << " of 2)";
	// The last message received was the timestamp-prefixed one; its fields must match.
	ASSERT_TRUE(lastChat == outgoingTs)
		<< "Live timestamp ChatMessage fields mismatch: '" << lastChat.text << "'/" << lastChat.channel;
	// Sender identity: the handler saw the client's GUID.
	ASSERT_EQ(lastChatGuid, clientGuid) << "Sender.guid() did not match the client GUID";
}
