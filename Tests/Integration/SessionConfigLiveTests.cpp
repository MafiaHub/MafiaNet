/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string.h>
#include <string>
#include <vector>

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

	// Drain everything a peer receives over a fixed window and keep the ids.
	//
	// Negative assertions must not be built out of successive SawWithin() probes: that helper discards
	// every packet that is not the one it is waiting for, so the first probe swallows the very packet a
	// later probe looks for and the test then passes for the wrong reason. Collect once, assert after.
	std::vector<int> CollectIds(RakPeerInterface *wanted, RakPeerInterface *alsoPump, int windowMs)
	{
		std::vector<int> ids;
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)windowMs)
		{
			Packet *p;
			for (p = wanted->Receive(); p; wanted->DeallocatePacket(p), p = wanted->Receive())
				ids.push_back((int)p->data[0]);
			if (alsoPump)
			{
				for (p = alsoPump->Receive(); p; alsoPump->DeallocatePacket(p), p = alsoPump->Receive())
					;
			}
			RakSleep(15);
		}
		return ids;
	}

	bool Contains(const std::vector<int> &ids, int id)
	{
		for (size_t i = 0; i < ids.size(); ++i)
		{
			if (ids[i] == id)
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

	// Collect everything the server sees for long enough that the DISCONNECT_ON_NO_ACK teardown has
	// completed -- the refusal is sent reliably, so the slot closes once the client acks it. The server
	// was never told this peer connected, so it must be told nothing at all about it going away: no
	// ID_NEW_INCOMING_CONNECTION, and equally no close notification for a connection that never was.
	const std::vector<int> serverSaw = CollectIds(server, client, 3000);

	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION))
		<< "server reported a connection it had rejected";
	EXPECT_FALSE(Contains(serverSaw, ID_DISCONNECTION_NOTIFICATION))
		<< "server was notified of a disconnect for a connection it never reported";
	EXPECT_FALSE(Contains(serverSaw, ID_CONNECTION_LOST))
		<< "server was notified of a lost connection it never reported";
	EXPECT_FALSE(Contains(serverSaw, ID_CONNECTION_ATTEMPT_FAILED))
		<< "server was told an inbound connection attempt failed; that is a connecting-side message";
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

// Role binding: the session-handshake replies are server->client messages, so a malicious client
// must not be able to push them at a listening server. ID_SESSION_CONFIG_REJECTED is the dangerous
// one -- unbound, it lets a client inject ID_CONNECTION_ATTEMPT_FAILED (a packet an application only
// ever expects for its OWN outbound connects) into the server's receive queue.
TEST_F(SessionConfigLive, ServerIgnoresSessionRepliesSentByAClient)
{
	server->SetSessionConfigInteractive(true);

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	const RakNetGUID clientGuid = request->guid;
	server->DeallocatePacket(request);

	// The server is now parked in EXCHANGING_SESSION_DATA awaiting a decision. Send it both replies
	// from the client, in the wrong direction.
	const char rejected[] = "\x00" "spoofed";
	MafiaNet::BitStream rejectBs;
	rejectBs.Write((MessageID)ID_SESSION_CONFIG_REJECTED);
	rejectBs.Write("spoofed", 7);
	client->Send(&rejectBs, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);

	MafiaNet::BitStream configBs;
	configBs.Write((MessageID)ID_SESSION_CONFIG);
	configBs.Write("spoofed-config", 14);
	client->Send(&configBs, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);
	(void)rejected;

	EXPECT_FALSE(SawWithin(server, ID_CONNECTION_ATTEMPT_FAILED, client, 700))
		<< "a client forged ID_CONNECTION_ATTEMPT_FAILED into the server's queue";
	EXPECT_FALSE(SawWithin(server, ID_NEW_INCOMING_CONNECTION, client, 300))
		<< "a client completed the server's half of the handshake by replying to itself";

	// The legitimate path must still work afterwards.
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int)strlen(kServerPayload));

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr) << "the spoof attempt broke the real handshake";
	server->DeallocatePacket(incoming);
}

// The stored payload is arbitrary attacker-controlled bytes. It is kept NUL-terminated past the
// reported length so an application that reaches for a C-string API cannot run off the buffer.
TEST_F(SessionConfigLive, RemotePayloadIsNulTerminatedPastItsLength)
{
	const char payload[] = "no-trailing-nul";
	server->SetSessionConfig(payload, (unsigned int)strlen(payload));

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr);
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);

	unsigned int length = 0;
	const char *config = client->GetRemoteSessionConfig(serverGuid, &length);
	ASSERT_NE(config, nullptr);
	ASSERT_EQ(length, (unsigned int)strlen(payload));
	// The terminator is past the reported length, so it never changes what the length means.
	EXPECT_EQ(config[length], '\0');
	EXPECT_EQ(strlen(config), (size_t)length);
}

// Admission control must see peers that are still running the session handshake. They already own a
// slot, so counting only CONNECTED peers would let clients that stall the handshake push the real
// total past SetMaximumIncomingConnections() -- and do it invisibly, since the application is never
// told those peers exist.
TEST_F(SessionConfigLive, StalledHandshakeStillConsumesAnIncomingSlot)
{
	server->SetSessionConfigInteractive(true); // never answered, so the first client parks mid-handshake

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(8, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(1); // exactly one incoming slot
	server->SetTimeoutTime(60000, UNASSIGNED_SYSTEM_ADDRESS); // outlive the test, so the stall persists

	SocketDescriptor clientSd(0, "127.0.0.1");
	ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);

	const unsigned short port = server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	server->DeallocatePacket(request);
	// The first client now occupies the only slot while parked in EXCHANGING_SESSION_DATA.

	RakPeerInterface *second = RakPeerInterface::GetInstance();
	ASSERT_NE(second, nullptr);
	SocketDescriptor secondSd(0, "127.0.0.1");
	ASSERT_EQ(second->Startup(1, &secondSd, 1), RAKNET_STARTED);
	ASSERT_EQ(second->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *full = PumpUntil(second, ID_NO_FREE_INCOMING_CONNECTIONS, server, kConnectTimeoutMs);
	const bool refused = full != nullptr;
	if (full)
		second->DeallocatePacket(full);

	second->Shutdown(100);
	RakPeerInterface::DestroyInstance(second);

	EXPECT_TRUE(refused) << "a peer stalled in the session handshake did not count against the incoming limit";
}

namespace
{
	const int kUserMessageId = ID_USER_PACKET_ENUM + 1;

	// Drive a complete connection lifecycle and assert every stage behaves identically whether or not a
	// session payload is configured. The handshake sits in front of everything else a peer does, so the
	// question is not only "does the payload arrive" but "does normal traffic, teardown and reconnect
	// still work with it in the path".
	//
	// The reconnect leg is the load-bearing one: it reuses the server's slot, so a payload that outlived
	// its connection, or session state that was not cleared on teardown, shows up here and nowhere else.
	//
	// The parameter is a plain bool rather than a struct so ctest renders a readable test name; gtest
	// appends the printed parameter to the discovered name, and a struct prints as "1-byte object <00>".
} // namespace

class SessionConfigPipeline : public SessionConfigLive, public ::testing::WithParamInterface<bool>
{
};

TEST_P(SessionConfigPipeline, FullConnectionLifecycleBehavesIdentically)
{
	const bool withConfig = GetParam();
	const unsigned int expectedServerLen = withConfig ? (unsigned int)strlen(kServerPayload) : 0u;
	const unsigned int expectedClientLen = withConfig ? (unsigned int)strlen(kClientPayload) : 0u;

	if (withConfig)
	{
		server->SetSessionConfig(kServerPayload, (unsigned int)strlen(kServerPayload));
		client->SetSessionConfig(kClientPayload, (unsigned int)strlen(kClientPayload));
	}

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	// ---- stage 1: both sides report the connection -------------------------------------------------
	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr) << "client never connected";
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr) << "server never reported the connection";
	const RakNetGUID clientGuid = incoming->guid;
	server->DeallocatePacket(incoming);

	unsigned int length = 12345;
	client->GetRemoteSessionConfig(serverGuid, &length);
	EXPECT_EQ(length, expectedServerLen);
	length = 12345;
	server->GetRemoteSessionConfig(clientGuid, &length);
	EXPECT_EQ(length, expectedClientLen);

	EXPECT_EQ(client->GetConnectionState(serverGuid), IS_CONNECTED);
	EXPECT_EQ(server->GetConnectionState(clientGuid), IS_CONNECTED);

	// ---- stage 2: ordinary traffic flows both ways -------------------------------------------------
	{
		MafiaNet::BitStream up;
		up.Write((MessageID)kUserMessageId);
		up.Write("client-to-server", 16);
		client->Send(&up, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);

		Packet *got = PumpUntil(server, kUserMessageId, client, kConnectTimeoutMs);
		ASSERT_NE(got, nullptr) << "server never received client traffic";
		ASSERT_EQ(got->length, (unsigned int)(1 + 16));
		EXPECT_EQ(memcmp(got->data + 1, "client-to-server", 16), 0);
		server->DeallocatePacket(got);
	}
	{
		MafiaNet::BitStream down;
		down.Write((MessageID)kUserMessageId);
		down.Write("server-to-client", 16);
		server->Send(&down, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);

		Packet *got = PumpUntil(client, kUserMessageId, server, kConnectTimeoutMs);
		ASSERT_NE(got, nullptr) << "client never received server traffic";
		ASSERT_EQ(got->length, (unsigned int)(1 + 16));
		EXPECT_EQ(memcmp(got->data + 1, "server-to-client", 16), 0);
		client->DeallocatePacket(got);
	}

	// ---- stage 3: clean teardown still notifies ----------------------------------------------------
	// This connection WAS reported, so unlike a rejected peer it must produce a disconnect notification.
	client->CloseConnection(serverGuid, true);

	Packet *bye = PumpUntil(server, ID_DISCONNECTION_NOTIFICATION, client, kConnectTimeoutMs);
	ASSERT_NE(bye, nullptr) << "server was not notified of a clean disconnect";
	server->DeallocatePacket(bye);

	// ---- stage 4: reconnect over the reused slot ---------------------------------------------------
	// CloseConnection is asynchronous on the closing side as well, so the local slot is still occupied
	// for a moment after the notification reaches the peer. Reconnecting before it frees returns
	// ALREADY_CONNECTED_TO_ENDPOINT; wait for the teardown rather than racing it.
	{
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)kConnectTimeoutMs)
		{
			const ConnectionState state = client->GetConnectionState(serverGuid);
			if (state == IS_NOT_CONNECTED || state == IS_DISCONNECTED)
				break;
			Packet *drain;
			for (drain = client->Receive(); drain; client->DeallocatePacket(drain), drain = client->Receive())
				;
			for (drain = server->Receive(); drain; server->DeallocatePacket(drain), drain = server->Receive())
				;
			RakSleep(15);
		}
	}

	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *accepted2 = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted2, nullptr) << "client could not reconnect";
	const RakNetGUID serverGuid2 = accepted2->guid;
	client->DeallocatePacket(accepted2);

	Packet *incoming2 = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming2, nullptr) << "server did not report the reconnection";
	const RakNetGUID clientGuid2 = incoming2->guid;
	server->DeallocatePacket(incoming2);

	// The payload must be freshly delivered for the new connection -- neither stale from the previous
	// one nor lost because teardown cleared it and nothing repopulated it.
	length = 12345;
	const char *serverCfg = client->GetRemoteSessionConfig(serverGuid2, &length);
	EXPECT_EQ(length, expectedServerLen) << "server payload wrong after reconnect";
	if (withConfig)
	{
		ASSERT_NE(serverCfg, nullptr);
		EXPECT_EQ(memcmp(serverCfg, kServerPayload, expectedServerLen), 0);
	}

	length = 12345;
	const char *clientCfg = server->GetRemoteSessionConfig(clientGuid2, &length);
	EXPECT_EQ(length, expectedClientLen) << "client payload wrong after reconnect";
	if (withConfig)
	{
		ASSERT_NE(clientCfg, nullptr);
		EXPECT_EQ(memcmp(clientCfg, kClientPayload, expectedClientLen), 0);
	}

	// ---- stage 5: traffic still flows on the reconnected session -----------------------------------
	MafiaNet::BitStream again;
	again.Write((MessageID)kUserMessageId);
	again.Write("after-reconnect!", 16);
	client->Send(&again, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);

	Packet *got = PumpUntil(server, kUserMessageId, client, kConnectTimeoutMs);
	ASSERT_NE(got, nullptr) << "traffic did not flow after reconnect";
	EXPECT_EQ(memcmp(got->data + 1, "after-reconnect!", 16), 0);
	server->DeallocatePacket(got);
}

INSTANTIATE_TEST_SUITE_P(WithAndWithoutSessionConfig, SessionConfigPipeline,
	::testing::Values(false, true),
	[](const ::testing::TestParamInfo<bool> &info) {
		return info.param ? "WithSessionConfig" : "WithoutSessionConfig";
	});
