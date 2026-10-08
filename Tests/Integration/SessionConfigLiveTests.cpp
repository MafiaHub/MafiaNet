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

#include "mafianet/RakPeer.h"
#include "mafianet/RakPeerInterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/RakSleep.h"
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

	// Waits for one packet on each of two peers at once, for the stages where both sides are notified
	// of the same event. Pumping them one at a time with PumpUntil() discards the other peer's packets
	// while waiting on the first, so whichever notification lands first on the "wrong" peer is thrown
	// away and the next wait times out. Returns false on timeout; on success both packets are the
	// caller's to deallocate.
	bool PumpUntilBoth(RakPeerInterface *first, int firstId, Packet **firstOut, RakPeerInterface *second, int secondId, Packet **secondOut, int timeoutMs)
	{
		*firstOut = 0;
		*secondOut = 0;
		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < (TimeMS)timeoutMs)
		{
			Packet *p;
			for (p = first->Receive(); p; p = first->Receive())
			{
				if (*firstOut == 0 && p->data[0] == (unsigned char)firstId)
					*firstOut = p;
				else
					first->DeallocatePacket(p);
			}
			for (p = second->Receive(); p; p = second->Receive())
			{
				if (*secondOut == 0 && p->data[0] == (unsigned char)secondId)
					*secondOut = p;
				else
					second->DeallocatePacket(p);
			}
			if (*firstOut && *secondOut)
				return true;
			RakSleep(15);
		}
		if (*firstOut)
			first->DeallocatePacket(*firstOut);
		if (*secondOut)
			second->DeallocatePacket(*secondOut);
		*firstOut = 0;
		*secondOut = 0;
		return false;
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

	// Injects application-layer bytes onto an established connection while bypassing the public Send()
	// handshake gate, standing in for a peer that does not respect the protocol.
	//
	// Needed because the gates added for un-accepted peers mask each other between two conforming peers:
	// the sender's broadcast filter and directed-send check mean nothing reaches the wire, so a negative
	// assertion on the receiver holds whether or not the receiving-side check exists. Injecting below the
	// gate is the only way to prove the receiving side actually rejects what it is sent.
	//
	// Never instantiated. RakPeer::SendBuffered is protected and a derived type is the only way to reach
	// it, but the peer itself must still come from RakPeerInterface::GetInstance() so that it is
	// allocated inside the library with the library's own layout. Constructing a RakPeer-derived object
	// in this translation unit would size it with THIS unit's view of the class, and a mismatch there is
	// silent memory corruption rather than a compile error.
	//
	// SendBuffered rather than SendImmediate: it is the layer the public Send() drops into once its own
	// checks pass, so injected traffic still crosses the network thread exactly like real traffic.
	class SendGateBypass : public RakPeer
	{
	public:
		static void Inject(RakPeerInterface *peer, const MafiaNet::BitStream &bs, const AddressOrGUID &target)
		{
			static_cast<SendGateBypass *>(static_cast<RakPeer *>(peer))->SendBuffered(
				(const char *)bs.GetData(), bs.GetNumberOfBitsUsed(), MafiaNet::Priority::Immediate,
				MafiaNet::Reliability::ReliableOrdered, 0, target, false,
				RemoteSystemStruct::NO_ACTION, 0);
		}
	};

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

		// A third peer for tests that need one. Fixture-owned rather than local, so a failed ASSERT_
		// still destroys it -- a leaked peer keeps a socket bound and a network thread alive for the rest
		// of the process, which in a serial suite shows up as an unrelated later test failing to bind.
		RakPeerInterface *MakeExtraPeer()
		{
			EXPECT_EQ(extra, nullptr) << "only one extra peer is supported";
			extra = RakPeerInterface::GetInstance();
			return extra;
		}

		void TearDown() override
		{
			if (extra)
			{
				extra->Shutdown(100);
				RakPeerInterface::DestroyInstance(extra);
				extra = 0;
			}
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
		RakPeerInterface *extra = 0;
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
	// The forged replies go through SendGateBypass deliberately. A conforming client cannot send these
	// at all -- its own broadcast filter and directed-send gate stop them before the wire -- so driving
	// this through the public Send() would make every assertion below hold even with the server-side
	// role checks deleted. What is under test is the SERVER's rejection, so the client has to transmit.
	server->SetSessionConfigInteractive(true);

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	const RakNetGUID clientGuid = request->guid;
	server->DeallocatePacket(request);
	// The server is parked in EXCHANGING_SESSION_DATA awaiting a decision. Forge both server-to-client
	// replies back at it, in the wrong direction.

	// The client holds no packet carrying the server's guid -- ID_CONNECTION_REQUEST_ACCEPTED is
	// withheld precisely because the handshake has not finished -- so address the server by endpoint.
	SystemAddress serverAddr;
	serverAddr.SetBinaryAddress("127.0.0.1");
	serverAddr.SetPortHostOrder(port);

	const char reason[] = "spoofed";
	MafiaNet::BitStream rejectBs;
	rejectBs.Write((MessageID)ID_SESSION_CONFIG_REJECTED);
	rejectBs.Write(reason, (unsigned int)strlen(reason));
	SendGateBypass::Inject(client, rejectBs, serverAddr);

	MafiaNet::BitStream configBs;
	configBs.Write((MessageID)ID_SESSION_CONFIG);
	configBs.Write("spoofed-config", 14);
	SendGateBypass::Inject(client, configBs, serverAddr);

	const std::vector<int> serverSaw = CollectIds(server, client, 1500);

	// ID_SESSION_CONFIG_REJECTED is the dangerous one: unbound, it becomes ID_CONNECTION_ATTEMPT_FAILED
	// on the receiver, a packet an application only ever expects for its OWN outbound connects.
	EXPECT_FALSE(Contains(serverSaw, ID_CONNECTION_ATTEMPT_FAILED))
		<< "a client forged ID_CONNECTION_ATTEMPT_FAILED into a listening server's queue";
	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION))
		<< "a client completed the server's half of the handshake by replying to itself";
	EXPECT_FALSE(Contains(serverSaw, ID_DISCONNECTION_NOTIFICATION))
		<< "the forged replies tore down a connection that was never reported";

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

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	SocketDescriptor secondSd(0, "127.0.0.1");
	ASSERT_EQ(second->Startup(1, &secondSd, 1), RAKNET_STARTED);
	ASSERT_EQ(second->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *full = PumpUntil(second, ID_NO_FREE_INCOMING_CONNECTIONS, server, kConnectTimeoutMs);
	ASSERT_NE(full, nullptr) << "a peer stalled in the session handshake did not count against the incoming limit";
	second->DeallocatePacket(full);
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
	Packet *accepted = 0;
	Packet *incoming = 0;
	ASSERT_TRUE(PumpUntilBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, &accepted, server, ID_NEW_INCOMING_CONNECTION, &incoming, kConnectTimeoutMs)) << "the connection was not reported on both sides";
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);

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

	Packet *accepted2 = 0;
	Packet *incoming2 = 0;
	ASSERT_TRUE(PumpUntilBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, &accepted2, server, ID_NEW_INCOMING_CONNECTION, &incoming2, kConnectTimeoutMs)) << "the reconnection was not reported on both sides";
	const RakNetGUID serverGuid2 = accepted2->guid;
	client->DeallocatePacket(accepted2);

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

// Application traffic must not cross a connection the application has not been told about.
//
// Three defences, and they are NOT equally testable, so be precise about what this asserts:
//
//   directed send  - Send() to an un-accepted peer returns 0. Directly observable, asserted below.
//   broadcast      - the fan-out skips peers mid-handshake.
//   inbound        - application data from a peer mid-handshake is dropped instead of delivered.
//
// The last two cannot be exercised against each other by two conforming peers: the sender's
// broadcast filter means nothing reaches the wire, and even if it did the receiver's inbound drop
// would discard it. Each masks the other, so an assertion on either would pass vacuously whether or
// not the code is present -- which is worse than no assertion, because it reads like coverage. They
// are wire-level defences against a peer that does NOT respect the protocol (an old client, a custom
// implementation, an attacker), which this harness cannot synthesise. Asserted here instead: the
// state the connection is in, that no connection is reported, and that everything works after accept.
TEST_F(SessionConfigLive, ApplicationTrafficIsBlockedUntilTheHandshakeCompletes)
{
	server->SetSessionConfigInteractive(true);

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	const RakNetGUID clientGuid = request->guid;
	server->DeallocatePacket(request);
	// Both peers are now parked in EXCHANGING_SESSION_DATA awaiting the server's decision.

	EXPECT_EQ(server->GetConnectionState(clientGuid), IS_CONNECTING)
		<< "a peer mid-handshake must not read as connected";

	// The application only holds this guid because interactive mode surfaced the request to it. Sending
	// to it would hand data to a peer it has not accepted and may still refuse.
	MafiaNet::BitStream world;
	world.Write((MessageID)kUserMessageId);
	world.Write("world-state-leak", 16);
	EXPECT_EQ(server->Send(&world, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, clientGuid, false), 0u)
		<< "a directed send reached a peer the application has not accepted";

	const std::vector<int> serverSaw = CollectIds(server, client, 700);
	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION))
		<< "server reported a connection before answering the session request";
	const std::vector<int> clientSaw = CollectIds(client, server, 400);
	EXPECT_FALSE(Contains(clientSaw, ID_CONNECTION_REQUEST_ACCEPTED))
		<< "client reported a connection before the handshake completed";

	// ---- once accepted, both directions work normally ----------------------------------------------
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int)strlen(kServerPayload));

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr);
	server->DeallocatePacket(incoming);
	Packet *accepted = PumpUntil(client, ID_CONNECTION_REQUEST_ACCEPTED, server, kConnectTimeoutMs);
	ASSERT_NE(accepted, nullptr);
	client->DeallocatePacket(accepted);

	// The same directed send that was refused a moment ago must now be accepted.
	MafiaNet::BitStream now;
	now.Write((MessageID)kUserMessageId);
	now.Write("after-accept-ok!", 16);
	EXPECT_NE(server->Send(&now, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, clientGuid, false), 0u)
		<< "a directed send was still refused after the handshake completed";

	Packet *got = PumpUntil(client, kUserMessageId, server, kConnectTimeoutMs);
	ASSERT_NE(got, nullptr) << "traffic did not flow after the handshake completed";
	EXPECT_EQ(memcmp(got->data + 1, "after-accept-ok!", 16), 0);
	client->DeallocatePacket(got);
}

// The receiving half of the traffic gate, provable only with the non-conforming injector: a peer that
// ignores its own send gate and pushes application data during the handshake must not have it
// delivered, because the application has not been told that connection exists.
//
// The sending half -- the broadcast fan-out skipping such peers -- has no equivalent test. Proving it
// means observing bytes on the wire, and any receiver capable of observing them also runs the inbound
// drop asserted here. It stays as defence for the case that matters, which is bytes leaving the
// machine toward a peer the application never accepted and may still refuse.
TEST_F(SessionConfigLive, InboundApplicationDataDuringHandshakeIsNotDelivered)
{
	// The receiving half of the traffic gate, provable only by bypassing the sender's own gate: a peer
	// that pushes application data during the handshake must not have it delivered, because the
	// application has not been told that connection exists.
	//
	// The sending half -- the broadcast fan-out skipping such peers -- has no equivalent test. Proving
	// it means observing bytes on the wire, and any receiver able to observe them also runs the inbound
	// drop asserted here. It stays as defence for the case that matters: bytes leaving the machine
	// toward a peer the application never accepted and may still refuse.
	server->SetSessionConfigInteractive(true);

	const unsigned short port = StartPeers();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, client, kConnectTimeoutMs);
	ASSERT_NE(request, nullptr);
	const RakNetGUID clientGuid = request->guid;
	server->DeallocatePacket(request);

	SystemAddress serverAddr;
	serverAddr.SetBinaryAddress("127.0.0.1");
	serverAddr.SetPortHostOrder(port);

	MafiaNet::BitStream early;
	early.Write((MessageID)kUserMessageId);
	early.Write("premature-inbound", 17);
	SendGateBypass::Inject(client, early, serverAddr);

	const std::vector<int> serverSaw = CollectIds(server, client, 1500);
	EXPECT_FALSE(Contains(serverSaw, kUserMessageId))
		<< "application data from a peer mid-handshake reached the application";
	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION))
		<< "server reported a connection before answering the session request";

	// Once accepted, the same peer's traffic is delivered normally -- the gate is about the connection
	// state, not about this peer being permanently distrusted.
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int)strlen(kServerPayload));

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr);
	server->DeallocatePacket(incoming);

	MafiaNet::BitStream now;
	now.Write((MessageID)kUserMessageId);
	now.Write("after-accept-ok!", 16);
	SendGateBypass::Inject(client, now, serverAddr);

	Packet *got = PumpUntil(server, kUserMessageId, client, kConnectTimeoutMs);
	ASSERT_NE(got, nullptr) << "traffic was still dropped after the handshake completed";
	EXPECT_EQ(memcmp(got->data + 1, "after-accept-ok!", 16), 0);
	server->DeallocatePacket(got);
}

// =========================================================================================================
// Pending-session pool, session timeout, status and abandonment (SetMaximumPendingSessions,
// SetSessionTimeout, SendSessionStatus, ID_SESSION_CONFIG_STATUS, ID_SESSION_CONFIG_ABANDONED).
//
// The pool is what lets an interactive server hold a connection while it decides without that connection
// taking a player's slot: a flood of stalled handshakes exhausts the pool, never the server.
// =========================================================================================================

namespace
{
	// Starts the server with an incoming limit and a pending pool, and the client plain. Returns the port.
	unsigned short StartPooled(RakPeerInterface *server, RakPeerInterface *client, unsigned short maxIncoming, unsigned short pool, unsigned short perAddress, TimeMS connectionTimeoutMs = 60000)
	{
		server->SetSessionConfigInteractive(true);
		server->SetMaximumPendingSessions(pool, perAddress);

		SocketDescriptor serverSd(0, "127.0.0.1");
		EXPECT_EQ(server->Startup((unsigned int)maxIncoming + pool, &serverSd, 1), RAKNET_STARTED);
		server->SetMaximumIncomingConnections(maxIncoming);
		server->SetTimeoutTime(connectionTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);

		SocketDescriptor clientSd(0, "127.0.0.1");
		EXPECT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);
		client->SetTimeoutTime(connectionTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);

		return server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
	}

	// Starts a further client and has it connect.
	void StartAndConnect(RakPeerInterface *peer, unsigned short port, TimeMS connectionTimeoutMs = 60000)
	{
		SocketDescriptor sd(0, "127.0.0.1");
		EXPECT_EQ(peer->Startup(1, &sd, 1), RAKNET_STARTED);
		peer->SetTimeoutTime(connectionTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
		EXPECT_EQ(peer->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);
	}

	// Waits for a session request on the server, pumping another peer meanwhile; hands back its guid.
	bool WaitForRequest(RakPeerInterface *server, RakPeerInterface *alsoPump, RakNetGUID &guid)
	{
		Packet *request = PumpUntil(server, ID_SESSION_CONFIG_REQUEST, alsoPump, kConnectTimeoutMs);
		if (!request)
			return false;
		guid = request->guid;
		server->DeallocatePacket(request);
		return true;
	}
} // namespace

// A handshake parked with the server undecided does not take the player slot: with the only slot's
// would-be owner stalled, another player still gets in. Without the pool the same setup refuses the
// second player (StalledHandshakeStillConsumesAnIncomingSlot).
TEST_F(SessionConfigLive, PendingPoolKeepsStalledHandshakesOutOfThePlayerSlots)
{
	const unsigned short port = StartPooled(server, client, 1, 4, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, client, stalledGuid)) << "first client never reached the handshake";
	// Deliberately never answered.

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	StartAndConnect(second, port);

	RakNetGUID secondGuid;
	ASSERT_TRUE(WaitForRequest(server, second, secondGuid)) << "the stalled handshake took the only player slot";
	EXPECT_NE(secondGuid, stalledGuid);

	server->AcceptSession(secondGuid, kServerPayload, (unsigned int)strlen(kServerPayload));

	Packet *accepted = 0;
	Packet *incoming = 0;
	ASSERT_TRUE(PumpUntilBoth(second, ID_CONNECTION_REQUEST_ACCEPTED, &accepted, server, ID_NEW_INCOMING_CONNECTION, &incoming, kConnectTimeoutMs))
		<< "the second player was not let in while the first sat in the handshake";
	EXPECT_EQ(incoming->guid, secondGuid);
	second->DeallocatePacket(accepted);
	server->DeallocatePacket(incoming);

	EXPECT_EQ(server->NumberOfConnections(), 1u) << "a peer still in the handshake must not read as connected";
	EXPECT_EQ(server->GetConnectionState(stalledGuid), IS_CONNECTING);
}

// The pool is itself bounded: once it is full a newcomer is refused at the transport, the same way a
// full server always refused one.
TEST_F(SessionConfigLive, PendingPoolRefusesNewcomersOnceFull)
{
	const unsigned short port = StartPooled(server, client, 8, 1, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, client, stalledGuid));

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	StartAndConnect(second, port);

	Packet *full = PumpUntil(second, ID_NO_FREE_INCOMING_CONNECTIONS, server, kConnectTimeoutMs);
	ASSERT_NE(full, nullptr) << "a full pending pool let another handshake start";
	second->DeallocatePacket(full);
}

// One address cannot fill the pool on its own. Both clients are on 127.0.0.1, so the second is the same
// address as the first.
TEST_F(SessionConfigLive, PendingPoolBoundsWhatOneAddressMayHold)
{
	const unsigned short port = StartPooled(server, client, 8, 8, 1);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, client, stalledGuid));

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	StartAndConnect(second, port);

	Packet *full = PumpUntil(second, ID_NO_FREE_INCOMING_CONNECTIONS, server, kConnectTimeoutMs);
	ASSERT_NE(full, nullptr) << "one address held more pending handshakes than its share";
	second->DeallocatePacket(full);
}

// A decision gives the pool slot back. A refused peer lingers only to deliver the refusal, and that must
// not keep the next newcomer out.
TEST_F(SessionConfigLive, PendingPoolSlotIsFreedByADecision)
{
	const unsigned short port = StartPooled(server, client, 8, 1, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID firstGuid;
	ASSERT_TRUE(WaitForRequest(server, client, firstGuid));
	server->RejectSession(firstGuid, "not on the whitelist");

	Packet *failed = PumpUntil(client, ID_CONNECTION_ATTEMPT_FAILED, server, kConnectTimeoutMs);
	ASSERT_NE(failed, nullptr);
	client->DeallocatePacket(failed);

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	StartAndConnect(second, port);

	// Collected rather than waited on with PumpUntil: the refused peer must not be reported as abandoned.
	std::vector<int> serverSaw;
	RakNetGUID secondGuid;
	bool gotRequest = false;
	const TimeMS entry = GetTimeMS();
	while (GetTimeMS() - entry < (TimeMS)kConnectTimeoutMs && !gotRequest)
	{
		Packet *p;
		for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
		{
			serverSaw.push_back((int)p->data[0]);
			if (p->data[0] == ID_SESSION_CONFIG_REQUEST)
			{
				gotRequest = true;
				secondGuid = p->guid;
			}
		}
		for (p = second->Receive(); p; second->DeallocatePacket(p), p = second->Receive())
			;
		for (p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
			;
		RakSleep(15);
	}
	ASSERT_TRUE(gotRequest) << "the refused peer still held the only pool slot";
	EXPECT_NE(secondGuid, firstGuid);
	EXPECT_FALSE(Contains(serverSaw, ID_SESSION_CONFIG_ABANDONED)) << "a peer the application refused was reported as abandoned";
}

// With a pool the handshake may start on a full server -- that is what makes a queue possible -- but an
// AcceptSession() that would overfill it is refused rather than honoured.
TEST_F(SessionConfigLive, AcceptSessionCannotOverfillTheIncomingLimit)
{
	const unsigned short port = StartPooled(server, client, 1, 4, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID firstGuid;
	ASSERT_TRUE(WaitForRequest(server, client, firstGuid));
	server->AcceptSession(firstGuid, 0, 0);

	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr);
	server->DeallocatePacket(incoming);
	// The only player slot is now taken.

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	StartAndConnect(second, port);

	RakNetGUID secondGuid;
	ASSERT_TRUE(WaitForRequest(server, second, secondGuid)) << "a full server with a pool refused to even start the handshake";

	server->AcceptSession(secondGuid, 0, 0);

	Packet *failed = PumpUntil(second, ID_CONNECTION_ATTEMPT_FAILED, server, kConnectTimeoutMs);
	ASSERT_NE(failed, nullptr) << "an accept past the incoming limit was honoured";
	second->DeallocatePacket(failed);

	const std::vector<int> serverSaw = CollectIds(server, second, 1000);
	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION)) << "the server reported a connection past its limit";
	EXPECT_EQ(server->NumberOfConnections(), 1u);
}

// SetMaximumPendingSessions(0, ...) restores the historic accounting exactly.
TEST_F(SessionConfigLive, ClearingThePoolRestoresTheDefaultAccounting)
{
	server->SetMaximumPendingSessions(4, 0);
	server->SetMaximumPendingSessions(0, 0);
	server->SetSessionConfigInteractive(true);

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(8, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(1);
	server->SetTimeoutTime(60000, UNASSIGNED_SYSTEM_ADDRESS);

	SocketDescriptor clientSd(0, "127.0.0.1");
	ASSERT_EQ(client->Startup(1, &clientSd, 1), RAKNET_STARTED);

	const unsigned short port = server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, client, stalledGuid));

	RakPeerInterface *second = MakeExtraPeer();
	ASSERT_NE(second, nullptr);
	StartAndConnect(second, port);

	Packet *full = PumpUntil(second, ID_NO_FREE_INCOMING_CONNECTIONS, server, kConnectTimeoutMs);
	ASSERT_NE(full, nullptr) << "without a pool a stalled handshake must still hold its incoming slot";
	second->DeallocatePacket(full);
}

// The session timeout replaces the connection timeout for the handshake, on both ends, and the server is
// told the request it was holding is gone.
TEST_F(SessionConfigLive, SessionTimeoutBoundsAnUnansweredHandshake)
{
	const TimeMS sessionTimeoutMs = 1000;
	server->SetSessionTimeout(sessionTimeoutMs);
	client->SetSessionTimeout(sessionTimeoutMs);
	// A connection timeout far longer than the test, so only the session timeout can end the attempt.
	const unsigned short port = StartPooled(server, client, 8, 4, 0, 60000);

	const TimeMS begin = GetTimeMS();
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, client, guid));

	Packet *failed = 0;
	Packet *abandoned = 0;
	ASSERT_TRUE(PumpUntilBoth(client, ID_CONNECTION_ATTEMPT_FAILED, &failed, server, ID_SESSION_CONFIG_ABANDONED, &abandoned, 15000))
		<< "the session timeout did not end the handshake on both ends";
	const TimeMS elapsed = GetTimeMS() - begin;
	EXPECT_EQ(abandoned->guid, guid);
	client->DeallocatePacket(failed);
	server->DeallocatePacket(abandoned);

	EXPECT_LT(elapsed, (TimeMS)15000) << "the handshake waited out something other than the session timeout";
	EXPECT_GE(elapsed, sessionTimeoutMs);
}

// Status reaches the client before any connection is reported, carries its payload, and restarts the
// session timeout on both ends: a peer kept informed for three timeouts' worth is still let in.
TEST_F(SessionConfigLive, SessionStatusReachesTheClientAndKeepsTheHandshakeAlive)
{
	const TimeMS sessionTimeoutMs = 1000;
	server->SetSessionTimeout(sessionTimeoutMs);
	client->SetSessionTimeout(sessionTimeoutMs);
	const unsigned short port = StartPooled(server, client, 8, 4, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, client, guid));

	std::vector<std::string> statuses;
	bool reportedEarly = false;
	bool failed = false;
	const TimeMS holdFor = sessionTimeoutMs * 3;
	const TimeMS begin = GetTimeMS();
	TimeMS lastStatus = 0;
	int sent = 0;
	while (GetTimeMS() - begin < holdFor)
	{
		if (sent == 0 || GetTimeMS() - lastStatus >= 400)
		{
			const std::string status = "queue:" + std::to_string(++sent);
			server->SendSessionStatus(guid, status.data(), (unsigned int)status.size());
			lastStatus = GetTimeMS();
		}
		Packet *p;
		for (p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
		{
			if (p->data[0] == ID_SESSION_CONFIG_STATUS)
				statuses.push_back(std::string((const char *)p->data + 1, p->length - 1));
			else if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
				reportedEarly = true;
			else if (p->data[0] == ID_CONNECTION_ATTEMPT_FAILED)
				failed = true;
		}
		for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
		{
			if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
				reportedEarly = true;
			else if (p->data[0] == ID_SESSION_CONFIG_ABANDONED)
				failed = true;
		}
		RakSleep(15);
	}

	EXPECT_FALSE(failed) << "status did not keep the handshake alive past the session timeout";
	EXPECT_FALSE(reportedEarly) << "a connection was reported while the server was still deciding";
	ASSERT_GE(statuses.size(), 3u) << "status did not reach the client";
	EXPECT_EQ(statuses[0], "queue:1");
	EXPECT_EQ(statuses[1], "queue:2");
	EXPECT_EQ(server->GetConnectionState(guid), IS_CONNECTING);

	server->AcceptSession(guid, kServerPayload, (unsigned int)strlen(kServerPayload));

	Packet *accepted = 0;
	Packet *incoming = 0;
	ASSERT_TRUE(PumpUntilBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, &accepted, server, ID_NEW_INCOMING_CONNECTION, &incoming, kConnectTimeoutMs))
		<< "a peer held with status was not let in when accepted";
	client->DeallocatePacket(accepted);
	server->DeallocatePacket(incoming);
}

// Status is for a peer awaiting a decision only; once accepted nothing more is sent.
TEST_F(SessionConfigLive, SessionStatusIsIgnoredOnceTheSessionIsDecided)
{
	const unsigned short port = StartPooled(server, client, 8, 4, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, client, guid));
	server->AcceptSession(guid, 0, 0);

	Packet *accepted = 0;
	Packet *incoming = 0;
	ASSERT_TRUE(PumpUntilBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, &accepted, server, ID_NEW_INCOMING_CONNECTION, &incoming, kConnectTimeoutMs));
	const RakNetGUID serverGuid = accepted->guid;
	client->DeallocatePacket(accepted);
	server->DeallocatePacket(incoming);

	server->SendSessionStatus(guid, "late", 4);
	const std::vector<int> clientSaw = CollectIds(client, server, 800);
	EXPECT_FALSE(Contains(clientSaw, ID_SESSION_CONFIG_STATUS)) << "status reached a peer that was already accepted";
	EXPECT_EQ(client->GetConnectionState(serverGuid), IS_CONNECTED);
}

// Role binding: status is a server-to-client message. A client pushing one at a server must not have it
// surfaced there.
TEST_F(SessionConfigLive, ServerIgnoresSessionStatusSentByAClient)
{
	const unsigned short port = StartPooled(server, client, 8, 4, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, client, guid));

	SystemAddress serverAddr;
	serverAddr.SetBinaryAddress("127.0.0.1");
	serverAddr.SetPortHostOrder(port);

	MafiaNet::BitStream forged;
	forged.Write((MessageID)ID_SESSION_CONFIG_STATUS);
	forged.Write("spoofed", 7);
	SendGateBypass::Inject(client, forged, serverAddr);

	const std::vector<int> serverSaw = CollectIds(server, client, 1200);
	EXPECT_FALSE(Contains(serverSaw, ID_SESSION_CONFIG_STATUS)) << "a client pushed a status into the server's queue";
	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION));

	// The real handshake is unaffected.
	server->AcceptSession(guid, 0, 0);
	Packet *incoming = PumpUntil(server, ID_NEW_INCOMING_CONNECTION, client, kConnectTimeoutMs);
	ASSERT_NE(incoming, nullptr);
	server->DeallocatePacket(incoming);
}

// A client that gives up while the server decides is reported to the server as abandoned, exactly once,
// and answering it afterwards is inert.
TEST_F(SessionConfigLive, ClientLeavingMidDecisionIsReportedAsAbandoned)
{
	const unsigned short port = StartPooled(server, client, 8, 4, 0);
	ASSERT_EQ(client->Connect("127.0.0.1", port, 0, 0), CONNECTION_ATTEMPT_STARTED);

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, client, guid));

	// Shutdown with a block duration sends the disconnection notification, so the server learns now
	// rather than after a timeout.
	client->Shutdown(300);

	Packet *abandoned = PumpUntil(server, ID_SESSION_CONFIG_ABANDONED, 0, kConnectTimeoutMs);
	ASSERT_NE(abandoned, nullptr) << "the server was never told the held request went away";
	EXPECT_EQ(abandoned->guid, guid);
	server->DeallocatePacket(abandoned);

	server->AcceptSession(guid, 0, 0);
	const std::vector<int> serverSaw = CollectIds(server, 0, 1200);
	EXPECT_FALSE(Contains(serverSaw, ID_SESSION_CONFIG_ABANDONED)) << "abandonment was reported twice";
	EXPECT_FALSE(Contains(serverSaw, ID_NEW_INCOMING_CONNECTION)) << "accepting an abandoned request reported a connection";
	EXPECT_FALSE(Contains(serverSaw, ID_CONNECTION_LOST)) << "a connection never reported was reported lost";
	EXPECT_FALSE(Contains(serverSaw, ID_DISCONNECTION_NOTIFICATION)) << "a connection never reported was reported closed";
	EXPECT_EQ(server->NumberOfConnections(), 0u);
}
