/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string.h>
#include <string>

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"

using namespace MafiaNet;

/*
Description:
Tests the session handshake: an opaque payload exchanged in both directions after the transport
connection is up but BEFORE either side reports a connection.

The load-bearing property is not that the payload arrives — it is that the connection packet is
withheld until it does. ID_CONNECTION_REQUEST_ACCEPTED means "the server's payload is in hand" and
ID_NEW_INCOMING_CONNECTION means "the client's payload is in hand", so an application cannot observe
a connection without its session data.

Success conditions:
- Static mode: both peers read the other's payload the moment their connection packet surfaces.
- Interactive mode: the server sees ID_SESSION_CONFIG_REQUEST and NO connection is reported on
  either side until it answers.
- AcceptSession completes the handshake and releases both connection packets.
- RejectSession produces ID_CONNECTION_ATTEMPT_FAILED on the client, carrying the reason, and no
  connection is reported anywhere.
- An interactive server that never answers times the handshake out rather than hanging.

Failure conditions: any of the above does not hold.
*/

namespace
{
	const int kConnectTimeoutMs = 15000;
	// Shortened from the 10s default so the never-answered case does not dominate suite runtime.
	const TimeMS kHandshakeTimeoutMs = 3000;

	// Drain a peer, returning the first packet with the given id, or 0 if the deadline passes.
	// Packets that are not the wanted id are discarded; both peers are pumped so a handshake that
	// needs traffic from either side can make progress.
	Packet *PumpUntil(RakPeerInterface *wanted, int wantedId, RakPeerInterface *alsoPump, int timeoutMs)
	{
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)timeoutMs)
		{
			Packet *p;
			for (p = wanted->Receive(); p; wanted->DeallocatePacket(p), p = wanted->Receive())
			{
				if (p->data[0] == (unsigned char)wantedId)
					return p; // caller deallocates
			}
			if (alsoPump)
			{
				for (p = alsoPump->Receive(); p; alsoPump->DeallocatePacket(p), p = alsoPump->Receive())
					;
			}
			RakSleep(15);
		}
		return 0;
	}

	// True if the id shows up within the window. Used for the negative assertions, where the point
	// is that a packet must NOT arrive.
	bool SawWithin(RakPeerInterface *wanted, int wantedId, RakPeerInterface *alsoPump, int windowMs)
	{
		Packet *p = PumpUntil(wanted, wantedId, alsoPump, windowMs);
		if (p)
		{
			wanted->DeallocatePacket(p);
			return true;
		}
		return false;
	}

	class SessionConfigLive : public ::testing::Test
	{
	public:
		void SetUp() override
		{
			server = RakPeerInterface::GetInstance();
			client = RakPeerInterface::GetInstance();
			ASSERT_NE(server, nullptr);
			ASSERT_NE(client, nullptr);
		}

		void TearDown() override
		{
			if (client)
			{
				client->Shutdown(100);
				RakPeerInterface::DestroyInstance(client);
				client = 0;
			}
			if (server)
			{
				server->Shutdown(100);
				RakPeerInterface::DestroyInstance(server);
				server = 0;
			}
		}

		// Start both peers on OS-assigned ports and return the server's port.
		unsigned short StartPeers()
		{
			SocketDescriptor serverSd(0, "127.0.0.1");
			EXPECT_EQ(server->Startup(8, &serverSd, 1), RAKNET_STARTED);
			server->SetMaximumIncomingConnections(8);
			server->SetTimeoutTime(kHandshakeTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);

			SocketDescriptor clientSd(0, "127.0.0.1");
			EXPECT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);
			client->SetTimeoutTime(kHandshakeTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);

			return server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
		}

		RakPeerInterface *server = 0;
		RakPeerInterface *client = 0;
	};

	const char kServerPayload[] = "{\"season\":\"winter\",\"map_file\":\"downtown.m2map\"}";
	const char kClientPayload[] = "{\"build\":\"m2o|1.2.3\"}";
} // namespace

// Static mode: no application code beyond SetSessionConfig. Each peer must be able to read the
// other's payload at the instant its connection packet surfaces.
TEST_F(SessionConfigLive, StaticExchangeDeliversBothPayloads)
{
	server->SetSessionConfig(kServerPayload, (unsigned int)strlen(kServerPayload));
	client->SetSessionConfig(kClientPayload, (unsigned int)strlen(kClientPayload));

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr) << "client never reported a connection";
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);

	// The payload must already be readable — that is the whole contract of the withheld packet.
	unsigned int length = 0;
	const char *fromServer = client->GetRemoteSessionConfig(serverGuid, &length);
	ASSERT_NE(fromServer, nullptr);
	ASSERT_EQ(length, (unsigned int)strlen(kServerPayload));
	EXPECT_EQ(memcmp(fromServer, kServerPayload, length), 0);

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr) << "server never reported a connection";
	const RakNetGUID clientGuid = incoming->guid;
	server->DeallocatePacket(incoming);

	length = 0;
	const char *fromClient = server->GetRemoteSessionConfig(clientGuid, &length);
	ASSERT_NE(fromClient, nullptr);
	ASSERT_EQ(length, (unsigned int)strlen(kClientPayload));
	EXPECT_EQ(memcmp(fromClient, kClientPayload, length), 0);
}

// A peer that configures no payload still completes the handshake; the remote simply reads none.
TEST_F(SessionConfigLive, EmptyPayloadsStillConnect)
{
	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr);
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);

	unsigned int length = 12345;
	client->GetRemoteSessionConfig(serverGuid, &length);
	EXPECT_EQ(length, 0u);

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr);
	server->DeallocatePacket(incoming);
}

// The load-bearing assertion. In interactive mode the server holds the decision, so neither peer may
// report a connection until it answers — proving the connection packets really are gated on the
// exchange and not merely racing it.
TEST_F(SessionConfigLive, InteractiveAcceptGatesBothConnectionPackets)
{
	server->SetSessionConfigInteractive(true);
	client->SetSessionConfig(kClientPayload, (unsigned int)strlen(kClientPayload));

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr) << "server never saw the session request";
	const RakNetGUID clientGuid = request->guid;

	// The client's payload rides the request itself.
	ASSERT_GT(request->length, (unsigned int)1);
	EXPECT_EQ(memcmp(request->data + 1, kClientPayload, strlen(kClientPayload)), 0);
	server->DeallocatePacket(request);

	// Nothing may be reported while the decision is outstanding.
	EXPECT_FALSE(SawWithin(server, ID_NEW_INCOMING_CONNECTION, client, 400))
		<< "server reported a connection before answering the session request";
	EXPECT_FALSE(SawWithin(client, ID_CONNECTION_REQUEST_ACCEPTED, server, 400))
		<< "client reported a connection before the server answered";

	server->AcceptSession(clientGuid, kServerPayload, (unsigned int)strlen(kServerPayload));

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr) << "AcceptSession did not release the server's connection packet";
	server->DeallocatePacket(incoming);

	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr) << "AcceptSession did not release the client's connection packet";
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);

	unsigned int length = 0;
	const char *fromServer = client->GetRemoteSessionConfig(serverGuid, &length);
	ASSERT_NE(fromServer, nullptr);
	ASSERT_EQ(length, (unsigned int)strlen(kServerPayload));
	EXPECT_EQ(memcmp(fromServer, kServerPayload, length), 0);
}

// A rejected peer must never see a connection, and must be told why.
TEST_F(SessionConfigLive, InteractiveRejectFailsTheConnectionAttempt)
{
	server->SetSessionConfigInteractive(true);

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	const RakNetGUID clientGuid = request->guid;
	server->DeallocatePacket(request);

	const char reason[] = "build mismatch";
	server->RejectSession(clientGuid, reason);

	Packet *failed = PumpUntil(client, ID_CONNECTION_ATTEMPT_FAILED, server, kConnectTimeoutMs);
	ASSERT_NE(failed, nullptr) << "rejected client was never told the attempt failed";
	// The reason rides after the id, matching the ID_DISCONNECTION_NOTIFICATION convention.
	ASSERT_EQ(failed->length, (unsigned int)(1 + strlen(reason)));
	EXPECT_EQ(memcmp(failed->data + 1, reason, strlen(reason)), 0);
	client->DeallocatePacket(failed);

	EXPECT_FALSE(SawWithin(server, ID_NEW_INCOMING_CONNECTION, client, 500))
		<< "server reported a connection it had rejected";
}

// An interactive server that never answers must not pin the slot or hang the client: acks keep
// flowing during the exchange, so the ordinary dead-connection detection would never fire.
TEST_F(SessionConfigLive, UnansweredHandshakeTimesOut)
{
	server->SetSessionConfigInteractive(true);

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	server->DeallocatePacket(request);

	// Deliberately never answer.
	Packet *failed = PumpUntil(client, ID_CONNECTION_ATTEMPT_FAILED, server, (int)kHandshakeTimeoutMs * 4);
	ASSERT_NE(failed, nullptr) << "an unanswered session handshake never timed out";
	client->DeallocatePacket(failed);

	EXPECT_NE(client->GetConnectionState(server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS)), IS_CONNECTED);
}
