/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The session handshake over the in-process SimulatedNetwork: whole RakPeer
 *  instances, no sockets, no threads, a virtual clock. See
 *  Tests/Support/SimulatedNetwork.h.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer.h"
#include "mafianet/peer_interface.h"
#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;

/*
Tests the session handshake: an opaque payload exchanged in both directions after the transport
connection is up but BEFORE either side reports a connection.

The load-bearing property is not that the payload arrives -- it is that the connection packet is
withheld until it does. ID_CONNECTION_REQUEST_ACCEPTED means "the server's payload is in hand" and
ID_NEW_INCOMING_CONNECTION means "the client's payload is in hand", so an application cannot observe
a connection without its session data.

These scenarios used to run over loopback with real threads (Tests/Integration/SessionConfigLiveTests.cpp)
and had to sample state racily and drain packets through helpers that could swallow the evidence a later
negative assertion needed. Here every peer runs on the test thread, one millisecond of virtual time per
update cycle, and everything every peer's Receive() returned stays in its inbox for the whole test. A
negative assertion therefore covers the entire scenario, not a window after a wait, and slot state is
read between every pair of update cycles rather than whenever the poll happened to come round.
*/

namespace
{
typedef SimulatedNetwork::Message Message;

const uint32_t kConnectTimeoutMs = 15000;
// Shortened from the 10s default, as in the loopback version.
const TimeMS kHandshakeTimeoutMs = 3000;
const MessageID kUserMessageId = ID_USER_PACKET_ENUM + 1;

const char kServerPayload[] = "{\"season\":\"winter\",\"map_file\":\"downtown.m2map\"}";
const char kClientPayload[] = "{\"build\":\"m2o|1.2.3\"}";

// Injects application-layer bytes onto an established connection while bypassing the public Send()
// handshake gate, standing in for a peer that does not respect the protocol.
//
// Needed because the gates for un-accepted peers mask each other between two conforming peers: the
// sender's broadcast filter and directed-send check mean nothing reaches the wire, so a negative
// assertion on the receiver holds whether or not the receiving-side check exists.
//
// Never instantiated: the peer comes from RakPeerInterface::GetInstance() (SimulatedNetwork::AddPeer
// uses it too), so it has the library's own layout. The derived type only names the protected member;
// the call goes through a pointer-to-member on the real RakPeer, because downcasting to this type
// would be undefined behaviour (UBSan's vptr check aborts on it).
//
// SendBuffered is the layer the public Send() drops into once its own checks pass, so the injected
// bytes are framed and sent by the next update cycle exactly like real traffic.
class SendGateBypass : public RakPeer
{
public:
	static void Inject(RakPeer *peer, const BitStream &bs, const AddressOrGUID &target,
	                   Reliability reliability = Reliability::ReliableOrdered, char orderingChannel = 0)
	{
		(peer->*&SendGateBypass::SendBuffered)(
		    (const char *) bs.GetData(), bs.GetNumberOfBitsUsed(), Priority::Immediate,
		    reliability, orderingChannel, target, false,
		    RemoteSystemStruct::NO_ACTION, 0, true, 0);
	}
};

// Reads a peer's own view of a connection slot. Never instantiated; see SendGateBypass.
class SlotInspector : public RakPeer
{
public:
	struct Snapshot
	{
		bool sawSlot = false;
		bool reported = false;
		unsigned int payloadLength = 0;
		bool exchanging = false;       // connectMode == EXCHANGING_SESSION_DATA
		unsigned int heldMessages = 0; // application messages held behind the handshake
		unsigned int heldBytes = 0;
		bool connectingSide = false;  // sessionConfigIsConnectingSide
		bool handlingRequest = false; // connectMode == HANDLING_CONNECTION_REQUEST
		bool weInitiated = false;     // weInitiatedTheConnection
	};

	static Snapshot Of(const RemoteSystemStruct &slot)
	{
		Snapshot out;
		out.sawSlot = true;
		out.reported = slot.connectionReportedToApplication;
		out.payloadLength = slot.remoteSessionConfigLength;
		out.exchanging = slot.connectMode == RemoteSystemStruct::EXCHANGING_SESSION_DATA;
		out.heldMessages = (unsigned int) slot.heldApplicationMessages.size();
		out.heldBytes = slot.heldApplicationBytes;
		out.connectingSide = slot.sessionConfigIsConnectingSide;
		out.handlingRequest = slot.connectMode == RemoteSystemStruct::HANDLING_CONNECTION_REQUEST;
		out.weInitiated = slot.weInitiatedTheConnection;
		return out;
	}

	// The active slot of the remote with this guid.
	static Snapshot SlotFor(const RakPeer *peer, const RakNetGUID &remote)
	{
		const RemoteSystemStruct *slots = peer->*&SlotInspector::remoteSystemList;
		const unsigned int slotCount = peer->*&SlotInspector::maximumNumberOfPeers;
		if (slots == nullptr)
			return Snapshot();
		for (unsigned int i = 0; i < slotCount; ++i)
			if (slots[i].isActive && slots[i].guid == remote)
				return Of(slots[i]);
		return Snapshot();
	}

	// The tests using this have exactly one remote, so the first active slot is it.
	static Snapshot FirstActiveSlot(const RakPeer *peer)
	{
		const RemoteSystemStruct *slots = peer->*&SlotInspector::remoteSystemList;
		const unsigned int slotCount = peer->*&SlotInspector::maximumNumberOfPeers;
		if (slots == nullptr)
			return Snapshot();
		for (unsigned int i = 0; i < slotCount; ++i)
			if (slots[i].isActive)
				return Of(slots[i]);
		return Snapshot();
	}
};

std::string IdList(const std::vector<Message> &inbox)
{
	std::string out;
	for (const Message &message : inbox)
	{
		if (!out.empty())
			out += ",";
		out += std::to_string((int) message.id);
	}
	return "[" + out + "]";
}

class SessionConfigSim : public ::testing::Test
{
protected:
	void TearDown() override
	{
		// Peers the fixture drives itself go before the network the fixture owns.
		for (RakPeer *peer : manual)
		{
			if (peer->IsActive())
				peer->Shutdown(0);
			RakPeerInterface::DestroyInstance(peer);
		}
		manual.clear();
	}

	// The server and first client of StartPeers() in the loopback version.
	RakPeer *AddServer(unsigned int maxConnections, unsigned short maxIncoming, TimeMS timeoutMs = kHandshakeTimeoutMs)
	{
		RakPeer *peer = net.AddPeer(maxConnections, maxIncoming);
		peer->SetTimeoutTime(timeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
		return peer;
	}

	RakPeer *AddClient(TimeMS timeoutMs = kHandshakeTimeoutMs)
	{
		RakPeer *peer = net.AddPeer(1);
		peer->SetTimeoutTime(timeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
		return peer;
	}

	// A client the fixture drives itself rather than SimulatedNetwork, for two things the network's
	// own peers cannot do: share a host with another peer (per-address limits), and Shutdown() in
	// the middle of a test (the network keeps updating its peers until it is destroyed).
	RakPeer *AddManualClient(const char *host, unsigned short port, TimeMS timeoutMs = kHandshakeTimeoutMs)
	{
		RakPeer *peer = static_cast<RakPeer *>(RakPeerInterface::GetInstance());
		manual.push_back(peer);
		peer->SetSocketFactory(&net);
		peer->SetManualUpdate(true);
		peer->SetInternalID(SystemAddress(host, port), 0);
		SocketDescriptor descriptor(port, host);
		EXPECT_EQ(RAKNET_STARTED, peer->Startup(1, &descriptor, 1));
		peer->SetTimeoutTime(timeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
		manualInbox[peer];
		return peer;
	}

	RakPeer *AddManualClientBeside(const RakPeer *neighbour, TimeMS timeoutMs = kHandshakeTimeoutMs)
	{
		const SystemAddress address = net.AddressOf(neighbour);
		char host[64];
		address.ToString(false, host, sizeof(host));
		return AddManualClient(host, (unsigned short) (address.GetPort() + 1), timeoutMs);
	}

	std::vector<Message> &Inbox(const RakPeer *peer)
	{
		auto it = manualInbox.find(peer);
		return it != manualInbox.end() ? it->second : net.Inbox(peer);
	}

	const Message *Find(const RakPeer *peer, MessageID id)
	{
		for (const Message &message : Inbox(peer))
			if (message.id == id)
				return &message;
		return nullptr;
	}

	bool Has(const RakPeer *peer, MessageID id) { return Find(peer, id) != nullptr; }

	size_t Count(const RakPeer *peer, MessageID id)
	{
		size_t n = 0;
		for (const Message &message : Inbox(peer))
			n += message.id == id ? 1 : 0;
		return n;
	}

	uint32_t NowMs() const { return (uint32_t) (net.Now() / 1000); }

	// One update cycle per millisecond for every peer.
	void Pump(uint32_t ms)
	{
		for (uint32_t i = 0; i < ms; ++i)
		{
			net.Step(1);
			for (RakPeer *peer : manual)
			{
				if (!peer->IsActive())
					continue;
				peer->RunManualUpdate();
				for (Packet *packet = peer->Receive(); packet; packet = peer->Receive())
				{
					Message message;
					message.id = packet->length ? packet->data[0] : 0;
					message.data.assign(packet->data, packet->data + packet->length);
					message.systemAddress = packet->systemAddress;
					message.guid = packet->guid;
					manualInbox[peer].push_back(std::move(message));
					peer->DeallocatePacket(packet);
				}
			}
		}
	}

	// Pump until pred() holds, checking after every update cycle.
	bool Until(const std::function<bool()> &pred, uint32_t maxMs)
	{
		for (uint32_t elapsed = 0; elapsed < maxMs; ++elapsed)
		{
			if (pred())
				return true;
			Pump(1);
		}
		return pred();
	}

	bool WaitFor(const RakPeer *peer, MessageID id, uint32_t maxMs = kConnectTimeoutMs)
	{
		return Until([&]
		             { return Has(peer, id); }, maxMs);
	}

	bool WaitForBoth(const RakPeer *a, MessageID aId, const RakPeer *b, MessageID bId, uint32_t maxMs = kConnectTimeoutMs)
	{
		return Until([&]
		             { return Has(a, aId) && Has(b, bId); }, maxMs);
	}

	// Waits for the session request on the server and hands back its guid.
	bool WaitForRequest(const RakPeer *server, RakNetGUID &guid)
	{
		if (!WaitFor(server, ID_SESSION_CONFIG_REQUEST))
			return false;
		guid = Find(server, ID_SESSION_CONFIG_REQUEST)->guid;
		return true;
	}

	ConnectionAttemptResult StartConnect(RakPeer *client, const RakPeer *server)
	{
		const SystemAddress target = net.AddressOf(server);
		char host[64];
		target.ToString(false, host, sizeof(host));
		return client->Connect(host, target.GetPort(), nullptr, 0);
	}

	// The pending-session pool setup: an interactive server with an incoming limit and a pool.
	RakPeer *AddPooledServer(unsigned short maxIncoming, unsigned short pool, unsigned short perAddress, TimeMS timeoutMs = 60000)
	{
		RakPeer *peer = AddServer((unsigned int) maxIncoming + pool, maxIncoming, timeoutMs);
		peer->SetSessionConfigInteractive(true);
		peer->SetMaximumPendingSessions(pool, perAddress);
		return peer;
	}

	SimulatedNetwork net;
	std::vector<RakPeer *> manual;
	std::map<const RakPeer *, std::vector<Message>> manualInbox;
};

void ExpectPayload(const char *got, unsigned int length, const char *expected, size_t expectedLength)
{
	ASSERT_NE(got, nullptr);
	ASSERT_EQ(length, (unsigned int) expectedLength);
	EXPECT_EQ(0, std::memcmp(got, expected, length));
}

void WriteUserMessage(BitStream &bs, const char *text)
{
	bs.Write(kUserMessageId);
	bs.Write(text, (unsigned int) std::strlen(text));
}

bool CarriesText(const Message *message, const char *text)
{
	const size_t n = std::strlen(text);
	return message && message->data.size() == 1 + n && std::memcmp(message->data.data() + 1, text, n) == 0;
}
} // namespace

// Static mode: no application code beyond SetSessionConfig. Each peer can read the other's payload at
// the instant its connection packet surfaces.
TEST_F(SessionConfigSim, StaticExchangeDeliversBothPayloads)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfig(kServerPayload, (unsigned int) std::strlen(kServerPayload));
	client->SetSessionConfig(kClientPayload, (unsigned int) std::strlen(kClientPayload));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	// Read the payload in the same cycle the connection packet surfaces, not some time after.
	bool clientChecked = false;
	bool serverChecked = false;
	ASSERT_TRUE(Until([&]
	                  {
		if (!clientChecked && Has(client, ID_CONNECTION_REQUEST_ACCEPTED))
		{
			unsigned int length = 0;
			const char *fromServer = client->GetRemoteSessionConfig(Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid, &length);
			ExpectPayload(fromServer, length, kServerPayload, std::strlen(kServerPayload));
			clientChecked = true;
		}
		if (!serverChecked && Has(server, ID_NEW_INCOMING_CONNECTION))
		{
			unsigned int length = 0;
			const char *fromClient = server->GetRemoteSessionConfig(Find(server, ID_NEW_INCOMING_CONNECTION)->guid, &length);
			ExpectPayload(fromClient, length, kClientPayload, std::strlen(kClientPayload));
			serverChecked = true;
		}
		return clientChecked && serverChecked; }, kConnectTimeoutMs))
	    << "client " << IdList(Inbox(client)) << " server " << IdList(Inbox(server));
}

// Static-mode gating: the server's connection packet is withheld while the library waits for the
// client's payload. In static mode correct code and code that released the packet at transport-up are
// indistinguishable to a poll, so this asserts the slot invariant "reported implies the whole payload is
// stored" between every pair of update cycles. The client payload is large enough to need many
// datagrams, so the handshake spends a long, observable time in the gated state.
TEST_F(SessionConfigSim, StaticModeWithholdsTheConnectionUntilTheClientPayloadArrives)
{
	std::string bigPayload;
	bigPayload.reserve(48000);
	while (bigPayload.size() < 48000)
		bigPayload.push_back((char) ('a' + (int) (bigPayload.size() % 26)));

	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfig(kServerPayload, (unsigned int) std::strlen(kServerPayload));
	client->SetSessionConfig(bigPayload.data(), (unsigned int) bigPayload.size());
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	unsigned int gatedCycles = 0;
	unsigned int violations = 0;
	bool reportedWithPayload = false;
	for (uint32_t elapsed = 0; elapsed < kConnectTimeoutMs && !reportedWithPayload; ++elapsed)
	{
		Pump(1);
		const SlotInspector::Snapshot snap = SlotInspector::FirstActiveSlot(server);
		const bool packetSurfaced = Has(server, ID_NEW_INCOMING_CONNECTION);
		if (snap.sawSlot && !snap.reported && snap.payloadLength == 0)
			++gatedCycles;
		if ((snap.reported || packetSurfaced) && snap.payloadLength != bigPayload.size())
			++violations;
		if (snap.reported && packetSurfaced && snap.payloadLength == bigPayload.size())
			reportedWithPayload = true;
	}

	EXPECT_EQ(0u, violations)
	    << "the server reported the connection while the client's session payload was still missing";
	EXPECT_GT(gatedCycles, 1u) << "never observed the handshake in its gated state, so this test proved nothing";
	ASSERT_TRUE(reportedWithPayload) << "the server never reported the connection with the client's payload in hand";

	// And the payload really did survive the split and reassembly.
	unsigned int length = 0;
	const char *fromClient = server->GetRemoteSessionConfig(Find(server, ID_NEW_INCOMING_CONNECTION)->guid, &length);
	ExpectPayload(fromClient, length, bigPayload.data(), bigPayload.size());
}

// A peer that configures no payload still completes the handshake; the remote simply reads none.
TEST_F(SessionConfigSim, EmptyPayloadsStillConnect)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(WaitForBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, server, ID_NEW_INCOMING_CONNECTION));

	unsigned int length = 12345;
	client->GetRemoteSessionConfig(Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid, &length);
	EXPECT_EQ(0u, length);
	length = 12345;
	server->GetRemoteSessionConfig(Find(server, ID_NEW_INCOMING_CONNECTION)->guid, &length);
	EXPECT_EQ(0u, length);
}

// The load-bearing assertion. In interactive mode the server holds the decision, so neither peer may
// report a connection until it answers.
TEST_F(SessionConfigSim, InteractiveAcceptGatesBothConnectionPackets)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfigInteractive(true);
	client->SetSessionConfig(kClientPayload, (unsigned int) std::strlen(kClientPayload));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid)) << "server never saw the session request";

	// The client's payload rides the request itself.
	const Message *request = Find(server, ID_SESSION_CONFIG_REQUEST);
	ASSERT_EQ(1 + std::strlen(kClientPayload), request->data.size());
	EXPECT_EQ(0, std::memcmp(request->data.data() + 1, kClientPayload, std::strlen(kClientPayload)));

	// Nothing may be reported while the decision is outstanding, checked after every cycle. The inboxes
	// hold everything since the connect, so this covers the wait above too.
	for (int i = 0; i < 400; ++i)
	{
		Pump(1);
		ASSERT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "server reported a connection before answering the session request";
		ASSERT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED)) << "client reported a connection before the server answered";
		ASSERT_FALSE(SlotInspector::FirstActiveSlot(server).reported);
	}

	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));

	ASSERT_TRUE(WaitFor(server, ID_NEW_INCOMING_CONNECTION)) << "AcceptSession did not release the server's connection packet";
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_REQUEST_ACCEPTED)) << "AcceptSession did not release the client's connection packet";

	unsigned int length = 0;
	const char *fromServer = client->GetRemoteSessionConfig(Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid, &length);
	ExpectPayload(fromServer, length, kServerPayload, std::strlen(kServerPayload));
}

// A rejected peer must never see a connection, and must be told why.
TEST_F(SessionConfigSim, InteractiveRejectFailsTheConnectionAttempt)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfigInteractive(true);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid));

	const char reason[] = "build mismatch";
	server->RejectSession(clientGuid, reason);

	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED)) << "rejected client was never told the attempt failed";
	// The reason rides after the id, matching the ID_DISCONNECTION_NOTIFICATION convention.
	const Message *failed = Find(client, ID_CONNECTION_ATTEMPT_FAILED);
	ASSERT_EQ(1 + std::strlen(reason), failed->data.size());
	EXPECT_EQ(0, std::memcmp(failed->data.data() + 1, reason, std::strlen(reason)));
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED));

	// Long enough for the DISCONNECT_ON_NO_ACK teardown to complete: the refusal is sent reliably, so the
	// slot closes once the client acks it. The server was never told this peer connected, so it must be
	// told nothing about it going away either.
	Pump(3000);
	EXPECT_FALSE(SlotInspector::FirstActiveSlot(server).sawSlot) << "the refused peer's slot was never released";
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "server reported a connection it had rejected";
	EXPECT_FALSE(Has(server, ID_DISCONNECTION_NOTIFICATION)) << "server was notified of a disconnect for a connection it never reported";
	EXPECT_FALSE(Has(server, ID_CONNECTION_LOST)) << "server was notified of a lost connection it never reported";
	EXPECT_FALSE(Has(server, ID_CONNECTION_ATTEMPT_FAILED)) << "server was told an inbound connection attempt failed; that is a connecting-side message";
}

// An interactive server that never answers must not pin the slot or hang the client: acks keep flowing
// during the exchange, so the ordinary dead-connection detection would never fire.
TEST_F(SessionConfigSim, UnansweredHandshakeTimesOut)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfigInteractive(true);
	const uint32_t begin = NowMs();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid));

	// Deliberately never answer.
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, kHandshakeTimeoutMs * 4)) << "an unanswered session handshake never timed out";
	EXPECT_GE(NowMs() - begin, (uint32_t) kHandshakeTimeoutMs) << "the attempt failed before the handshake timeout";
	EXPECT_LT(NowMs() - begin, (uint32_t) kHandshakeTimeoutMs * 2) << "the attempt outlived the handshake timeout";
	EXPECT_NE(IS_CONNECTED, client->GetConnectionState(net.AddressOf(server)));
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION));
}

// Role binding: the session-handshake replies are server->client messages, so a client must not be
// able to push them at a listening server. ID_SESSION_CONFIG_REJECTED is the dangerous one -- unbound,
// it lets a client inject ID_CONNECTION_ATTEMPT_FAILED into the server's receive queue.
TEST_F(SessionConfigSim, ServerIgnoresSessionRepliesSentByAClient)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfigInteractive(true);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid));
	// The server is parked in EXCHANGING_SESSION_DATA. Forge both server-to-client replies back at it.
	// The client holds no packet carrying the server's guid, so address the server by endpoint.
	const SystemAddress serverAddr = net.AddressOf(server);

	BitStream rejectBs;
	rejectBs.Write((MessageID) ID_SESSION_CONFIG_REJECTED);
	rejectBs.Write("spoofed", 7);
	SendGateBypass::Inject(client, rejectBs, serverAddr);

	BitStream configBs;
	configBs.Write((MessageID) ID_SESSION_CONFIG);
	configBs.Write("spoofed-config", 14);
	SendGateBypass::Inject(client, configBs, serverAddr);

	Pump(1500);

	EXPECT_FALSE(Has(server, ID_CONNECTION_ATTEMPT_FAILED)) << "a client forged ID_CONNECTION_ATTEMPT_FAILED into a listening server's queue";
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "a client completed the server's half of the handshake by replying to itself";
	EXPECT_FALSE(Has(server, ID_DISCONNECTION_NOTIFICATION)) << "the forged replies tore down a connection that was never reported";
	EXPECT_EQ(IS_CONNECTING, server->GetConnectionState(clientGuid));

	// The legitimate path must still work afterwards.
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(WaitFor(server, ID_NEW_INCOMING_CONNECTION)) << "the spoof attempt broke the real handshake";
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_REQUEST_ACCEPTED));
}

// The stored payload is arbitrary attacker-controlled bytes. It is kept NUL-terminated past the reported
// length so an application that reaches for a C-string API cannot run off the buffer.
TEST_F(SessionConfigSim, RemotePayloadIsNulTerminatedPastItsLength)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	const char payload[] = "no-trailing-nul";
	server->SetSessionConfig(payload, (unsigned int) std::strlen(payload));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_REQUEST_ACCEPTED));

	unsigned int length = 0;
	const char *config = client->GetRemoteSessionConfig(Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid, &length);
	ASSERT_NE(config, nullptr);
	ASSERT_EQ((unsigned int) std::strlen(payload), length);
	EXPECT_EQ('\0', config[length]);
	EXPECT_EQ((size_t) length, std::strlen(config));
}

// Admission control must see peers that are still running the session handshake: they already own a
// slot, so counting only CONNECTED peers would let stalled handshakes push past the incoming limit.
TEST_F(SessionConfigSim, StalledHandshakeStillConsumesAnIncomingSlot)
{
	RakPeer *server = AddServer(8, 1, 60000);
	server->SetSessionConfigInteractive(true); // never answered, so the first client parks mid-handshake
	RakPeer *client = AddClient();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, stalledGuid));

	RakPeer *second = AddClient();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(WaitFor(second, ID_NO_FREE_INCOMING_CONNECTIONS))
	    << "a peer stalled in the session handshake did not count against the incoming limit";
	EXPECT_EQ(1u, Count(server, ID_SESSION_CONFIG_REQUEST));
}

// A complete connection lifecycle behaves identically whether or not a session payload is configured.
// The reconnect leg is the load-bearing one: it reuses the server's slot, so a payload that outlived its
// connection, or session state not cleared on teardown, shows up here and nowhere else.
class SessionConfigSimPipeline : public SessionConfigSim, public ::testing::WithParamInterface<bool>
{
};

TEST_P(SessionConfigSimPipeline, FullConnectionLifecycleBehavesIdentically)
{
	const bool withConfig = GetParam();
	const unsigned int expectedServerLen = withConfig ? (unsigned int) std::strlen(kServerPayload) : 0u;
	const unsigned int expectedClientLen = withConfig ? (unsigned int) std::strlen(kClientPayload) : 0u;

	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	if (withConfig)
	{
		server->SetSessionConfig(kServerPayload, expectedServerLen);
		client->SetSessionConfig(kClientPayload, expectedClientLen);
	}

	// ---- stage 1: both sides report the connection --------------------------------------------------
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(WaitForBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, server, ID_NEW_INCOMING_CONNECTION))
	    << "the connection was not reported on both sides";
	const RakNetGUID serverGuid = Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid;
	const RakNetGUID clientGuid = Find(server, ID_NEW_INCOMING_CONNECTION)->guid;

	unsigned int length = 12345;
	client->GetRemoteSessionConfig(serverGuid, &length);
	EXPECT_EQ(expectedServerLen, length);
	length = 12345;
	server->GetRemoteSessionConfig(clientGuid, &length);
	EXPECT_EQ(expectedClientLen, length);
	EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(serverGuid));
	EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(clientGuid));

	// ---- stage 2: ordinary traffic flows both ways --------------------------------------------------
	{
		BitStream up;
		WriteUserMessage(up, "client-to-server");
		client->Send(&up, Priority::High, Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);
		ASSERT_TRUE(WaitFor(server, kUserMessageId)) << "server never received client traffic";
		EXPECT_TRUE(CarriesText(Find(server, kUserMessageId), "client-to-server"));
	}
	{
		BitStream down;
		WriteUserMessage(down, "server-to-client");
		server->Send(&down, Priority::High, Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);
		ASSERT_TRUE(WaitFor(client, kUserMessageId)) << "client never received server traffic";
		EXPECT_TRUE(CarriesText(Find(client, kUserMessageId), "server-to-client"));
	}

	// ---- stage 3: clean teardown still notifies -----------------------------------------------------
	// This connection WAS reported, so unlike a rejected peer it must produce a disconnect notification.
	client->CloseConnection(serverGuid, true);
	ASSERT_TRUE(WaitFor(server, ID_DISCONNECTION_NOTIFICATION)) << "server was not notified of a clean disconnect";

	// ---- stage 4: reconnect over the reused slot ----------------------------------------------------
	// CloseConnection is asynchronous on the closing side too: reconnecting before the local slot frees
	// returns ALREADY_CONNECTED_TO_ENDPOINT, so wait for the teardown.
	ASSERT_TRUE(Until([&]
	                  {
		const ConnectionState state = client->GetConnectionState(serverGuid);
		return state == IS_NOT_CONNECTED || state == IS_DISCONNECTED; }, kConnectTimeoutMs));

	Inbox(client).clear();
	Inbox(server).clear();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(WaitForBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, server, ID_NEW_INCOMING_CONNECTION))
	    << "the reconnection was not reported on both sides";
	const RakNetGUID serverGuid2 = Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid;
	const RakNetGUID clientGuid2 = Find(server, ID_NEW_INCOMING_CONNECTION)->guid;

	// The payload must be freshly delivered for the new connection -- neither stale from the previous one
	// nor lost because teardown cleared it and nothing repopulated it.
	length = 12345;
	const char *serverCfg = client->GetRemoteSessionConfig(serverGuid2, &length);
	EXPECT_EQ(expectedServerLen, length) << "server payload wrong after reconnect";
	if (withConfig)
		ExpectPayload(serverCfg, length, kServerPayload, expectedServerLen);

	length = 12345;
	const char *clientCfg = server->GetRemoteSessionConfig(clientGuid2, &length);
	EXPECT_EQ(expectedClientLen, length) << "client payload wrong after reconnect";
	if (withConfig)
		ExpectPayload(clientCfg, length, kClientPayload, expectedClientLen);

	// ---- stage 5: traffic still flows on the reconnected session ------------------------------------
	BitStream again;
	WriteUserMessage(again, "after-reconnect!");
	client->Send(&again, Priority::High, Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);
	ASSERT_TRUE(WaitFor(server, kUserMessageId)) << "traffic did not flow after reconnect";
	EXPECT_TRUE(CarriesText(Find(server, kUserMessageId), "after-reconnect!"));
}

INSTANTIATE_TEST_SUITE_P(WithAndWithoutSessionConfig, SessionConfigSimPipeline,
                         ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool> &info)
                         {
	                         return info.param ? "WithSessionConfig" : "WithoutSessionConfig";
                         });

// Application traffic must not cross a connection the application has not been told about. This one
// covers the directed-send half and the connection state; the inbound half is the next test.
TEST_F(SessionConfigSim, ApplicationTrafficIsBlockedUntilTheHandshakeCompletes)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfigInteractive(true);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid));
	EXPECT_EQ(IS_CONNECTING, server->GetConnectionState(clientGuid)) << "a peer mid-handshake must not read as connected";

	// The application only holds this guid because interactive mode surfaced the request to it.
	BitStream world;
	WriteUserMessage(world, "world-state-leak");
	EXPECT_EQ(0u, server->Send(&world, Priority::High, Reliability::ReliableOrdered, 0, clientGuid, false))
	    << "a directed send reached a peer the application has not accepted";

	Pump(700);
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "server reported a connection before answering the session request";
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED)) << "client reported a connection before the handshake completed";
	EXPECT_FALSE(Has(client, kUserMessageId)) << "the refused send reached the client anyway";

	// ---- once accepted, both directions work normally -----------------------------------------------
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(WaitForBoth(server, ID_NEW_INCOMING_CONNECTION, client, ID_CONNECTION_REQUEST_ACCEPTED));

	BitStream now;
	WriteUserMessage(now, "after-accept-ok!");
	EXPECT_NE(0u, server->Send(&now, Priority::High, Reliability::ReliableOrdered, 0, clientGuid, false))
	    << "a directed send was still refused after the handshake completed";
	ASSERT_TRUE(WaitFor(client, kUserMessageId)) << "traffic did not flow after the handshake completed";
	EXPECT_TRUE(CarriesText(Find(client, kUserMessageId), "after-accept-ok!"));
	EXPECT_EQ(1u, Count(client, kUserMessageId));
}

// The receiving half of the traffic gate, provable only by bypassing the sender's own gate: a peer that
// pushes application data during the handshake must not have it delivered.
TEST_F(SessionConfigSim, InboundApplicationDataDuringHandshakeIsNotDelivered)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	server->SetSessionConfigInteractive(true);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid));
	const SystemAddress serverAddr = net.AddressOf(server);

	BitStream early;
	WriteUserMessage(early, "premature-inbound");
	SendGateBypass::Inject(client, early, serverAddr);

	Pump(1500);
	EXPECT_FALSE(Has(server, kUserMessageId)) << "application data from a peer mid-handshake reached the application";
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "server reported a connection before answering the session request";

	// Once accepted, the same peer's traffic is delivered normally -- the gate is about the connection
	// state, not about this peer being permanently distrusted.
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(WaitFor(server, ID_NEW_INCOMING_CONNECTION));

	BitStream now;
	WriteUserMessage(now, "after-accept-ok!");
	SendGateBypass::Inject(client, now, serverAddr);
	ASSERT_TRUE(WaitFor(server, kUserMessageId)) << "traffic was still dropped after the handshake completed";
	EXPECT_TRUE(CarriesText(Find(server, kUserMessageId), "after-accept-ok!"))
	    << "the message dropped during the handshake was delivered late instead";
	EXPECT_EQ(1u, Count(server, kUserMessageId));
}

// =========================================================================================================
// Pending-session pool, session timeout, status and abandonment (SetMaximumPendingSessions,
// SetSessionTimeout, SendSessionStatus, ID_SESSION_CONFIG_STATUS, ID_SESSION_CONFIG_ABANDONED).
// =========================================================================================================

// A handshake parked with the server undecided does not take the player slot.
TEST_F(SessionConfigSim, PendingPoolKeepsStalledHandshakesOutOfThePlayerSlots)
{
	RakPeer *server = AddPooledServer(1, 4, 0);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, stalledGuid)) << "first client never reached the handshake";
	// Deliberately never answered.

	RakPeer *second = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(Until([&]
	                  { return Count(server, ID_SESSION_CONFIG_REQUEST) == 2; }, kConnectTimeoutMs))
	    << "the stalled handshake took the only player slot; second saw " << IdList(Inbox(second));
	const RakNetGUID secondGuid = Inbox(server).back().guid;
	ASSERT_EQ(ID_SESSION_CONFIG_REQUEST, Inbox(server).back().id);
	EXPECT_NE(stalledGuid, secondGuid);
	EXPECT_EQ(second->GetMyGUID(), secondGuid);

	server->AcceptSession(secondGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(WaitForBoth(second, ID_CONNECTION_REQUEST_ACCEPTED, server, ID_NEW_INCOMING_CONNECTION))
	    << "the second player was not let in while the first sat in the handshake";
	EXPECT_EQ(secondGuid, Find(server, ID_NEW_INCOMING_CONNECTION)->guid);
	EXPECT_EQ(1u, server->NumberOfConnections()) << "a peer still in the handshake must not read as connected";
	EXPECT_EQ(IS_CONNECTING, server->GetConnectionState(stalledGuid));
}

// The pool is itself bounded: once it is full a newcomer is refused at the transport.
TEST_F(SessionConfigSim, PendingPoolRefusesNewcomersOnceFull)
{
	RakPeer *server = AddPooledServer(8, 1, 0);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, stalledGuid));

	RakPeer *second = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(WaitFor(second, ID_NO_FREE_INCOMING_CONNECTIONS)) << "a full pending pool let another handshake start";
	EXPECT_EQ(1u, Count(server, ID_SESSION_CONFIG_REQUEST));
}

// One address cannot fill the pool on its own. The second client shares the first one's host.
TEST_F(SessionConfigSim, PendingPoolBoundsWhatOneAddressMayHold)
{
	RakPeer *server = AddPooledServer(8, 8, 1);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, stalledGuid));

	RakPeer *second = AddManualClientBeside(client, 60000);
	ASSERT_TRUE(net.AddressOf(second).EqualsExcludingPort(net.AddressOf(client)));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(WaitFor(second, ID_NO_FREE_INCOMING_CONNECTIONS)) << "one address held more pending handshakes than its share";
	EXPECT_EQ(1u, Count(server, ID_SESSION_CONFIG_REQUEST));

	// The limit is per address, not a smaller pool: a client on another host still gets in.
	RakPeer *third = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(third, server));
	ASSERT_TRUE(Until([&]
	                  { return Count(server, ID_SESSION_CONFIG_REQUEST) == 2; }, kConnectTimeoutMs))
	    << "third saw " << IdList(Inbox(third));
}

// A decision gives the pool slot back. A refused peer lingers only to deliver the refusal, and that must
// not keep the next newcomer out -- nor be reported as abandoned.
TEST_F(SessionConfigSim, PendingPoolSlotIsFreedByADecision)
{
	RakPeer *server = AddPooledServer(8, 1, 0);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID firstGuid;
	ASSERT_TRUE(WaitForRequest(server, firstGuid));
	server->RejectSession(firstGuid, "not on the whitelist");
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED));

	RakPeer *second = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(Until([&]
	                  { return Count(server, ID_SESSION_CONFIG_REQUEST) == 2; }, kConnectTimeoutMs))
	    << "the refused peer still held the only pool slot; second saw " << IdList(Inbox(second));
	EXPECT_EQ(second->GetMyGUID(), Inbox(server).back().guid);
	EXPECT_NE(firstGuid, Inbox(server).back().guid);
	EXPECT_FALSE(Has(server, ID_SESSION_CONFIG_ABANDONED)) << "a peer the application refused was reported as abandoned";
}

// With a pool the handshake may start on a full server, but an AcceptSession() that would overfill it is
// refused rather than honoured.
TEST_F(SessionConfigSim, AcceptSessionCannotOverfillTheIncomingLimit)
{
	RakPeer *server = AddPooledServer(1, 4, 0);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID firstGuid;
	ASSERT_TRUE(WaitForRequest(server, firstGuid));
	server->AcceptSession(firstGuid, nullptr, 0);
	ASSERT_TRUE(WaitFor(server, ID_NEW_INCOMING_CONNECTION));
	// The only player slot is now taken.

	RakPeer *second = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(Until([&]
	                  { return Count(server, ID_SESSION_CONFIG_REQUEST) == 2; }, kConnectTimeoutMs))
	    << "a full server with a pool refused to even start the handshake; second saw " << IdList(Inbox(second));
	const RakNetGUID secondGuid = Inbox(server).back().guid;

	server->AcceptSession(secondGuid, nullptr, 0);
	ASSERT_TRUE(WaitFor(second, ID_CONNECTION_ATTEMPT_FAILED)) << "an accept past the incoming limit was honoured";

	Pump(1000);
	EXPECT_EQ(1u, Count(server, ID_NEW_INCOMING_CONNECTION)) << "the server reported a connection past its limit";
	EXPECT_FALSE(Has(second, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(1u, server->NumberOfConnections());
}

// SetMaximumPendingSessions(0, ...) restores the historic accounting exactly.
TEST_F(SessionConfigSim, ClearingThePoolRestoresTheDefaultAccounting)
{
	RakPeer *server = AddServer(8, 1, 60000);
	server->SetMaximumPendingSessions(4, 0);
	server->SetMaximumPendingSessions(0, 0);
	server->SetSessionConfigInteractive(true);
	RakPeer *client = AddClient();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID stalledGuid;
	ASSERT_TRUE(WaitForRequest(server, stalledGuid));

	RakPeer *second = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(second, server));
	ASSERT_TRUE(WaitFor(second, ID_NO_FREE_INCOMING_CONNECTIONS)) << "without a pool a stalled handshake must still hold its incoming slot";
}

// The session timeout replaces the connection timeout for the handshake, on both ends, and the server is
// told the request it was holding is gone.
TEST_F(SessionConfigSim, SessionTimeoutBoundsAnUnansweredHandshake)
{
	const TimeMS sessionTimeoutMs = 1000;
	// A connection timeout far longer than the test, so only the session timeout can end the attempt.
	RakPeer *server = AddPooledServer(8, 4, 0, 60000);
	RakPeer *client = AddClient(60000);
	server->SetSessionTimeout(sessionTimeoutMs);
	client->SetSessionTimeout(sessionTimeoutMs);

	const uint32_t begin = NowMs();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, guid));

	ASSERT_TRUE(WaitForBoth(client, ID_CONNECTION_ATTEMPT_FAILED, server, ID_SESSION_CONFIG_ABANDONED))
	    << "the session timeout did not end the handshake on both ends";
	const uint32_t elapsed = NowMs() - begin;
	EXPECT_EQ(guid, Find(server, ID_SESSION_CONFIG_ABANDONED)->guid);
	EXPECT_EQ(1u, Count(server, ID_SESSION_CONFIG_ABANDONED));
	EXPECT_GE(elapsed, (uint32_t) sessionTimeoutMs);
	EXPECT_LT(elapsed, (uint32_t) sessionTimeoutMs * 2) << "the handshake waited out something other than the session timeout";
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION));
}

// Status reaches the client before any connection is reported, carries its payload, and restarts the
// session timeout on both ends: a peer kept informed for three timeouts' worth is still let in.
TEST_F(SessionConfigSim, SessionStatusReachesTheClientAndKeepsTheHandshakeAlive)
{
	const TimeMS sessionTimeoutMs = 1000;
	RakPeer *server = AddPooledServer(8, 4, 0);
	RakPeer *client = AddClient(60000);
	server->SetSessionTimeout(sessionTimeoutMs);
	client->SetSessionTimeout(sessionTimeoutMs);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, guid));

	std::vector<std::string> sentStatuses;
	const uint32_t begin = NowMs();
	uint32_t lastStatus = 0;
	while (NowMs() - begin < sessionTimeoutMs * 3)
	{
		if (sentStatuses.empty() || NowMs() - lastStatus >= 400)
		{
			sentStatuses.push_back("queue:" + std::to_string(sentStatuses.size() + 1));
			server->SendSessionStatus(guid, sentStatuses.back().data(), (unsigned int) sentStatuses.back().size());
			lastStatus = NowMs();
		}
		Pump(1);
	}
	Pump(50); // let the last status land

	EXPECT_FALSE(Has(client, ID_CONNECTION_ATTEMPT_FAILED)) << "status did not keep the handshake alive past the session timeout";
	EXPECT_FALSE(Has(server, ID_SESSION_CONFIG_ABANDONED)) << "status did not keep the handshake alive past the session timeout";
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED)) << "a connection was reported while the server was still deciding";
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "a connection was reported while the server was still deciding";

	std::vector<std::string> statuses;
	for (const Message &message : Inbox(client))
		if (message.id == ID_SESSION_CONFIG_STATUS)
			statuses.emplace_back((const char *) message.data.data() + 1, message.data.size() - 1);
	ASSERT_GE(sentStatuses.size(), 3u);
	EXPECT_EQ(sentStatuses, statuses) << "every status, in order, with its payload";
	EXPECT_EQ(IS_CONNECTING, server->GetConnectionState(guid));

	server->AcceptSession(guid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(WaitForBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, server, ID_NEW_INCOMING_CONNECTION))
	    << "a peer held with status was not let in when accepted";
}

// Status is for a peer awaiting a decision only; once accepted nothing more is sent.
TEST_F(SessionConfigSim, SessionStatusIsIgnoredOnceTheSessionIsDecided)
{
	RakPeer *server = AddPooledServer(8, 4, 0);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, guid));
	server->AcceptSession(guid, nullptr, 0);
	ASSERT_TRUE(WaitForBoth(client, ID_CONNECTION_REQUEST_ACCEPTED, server, ID_NEW_INCOMING_CONNECTION));
	const RakNetGUID serverGuid = Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid;

	server->SendSessionStatus(guid, "late", 4);
	Pump(800);
	EXPECT_FALSE(Has(client, ID_SESSION_CONFIG_STATUS)) << "status reached a peer that was already accepted";
	EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(serverGuid));
}

// Role binding: status is a server-to-client message. A client pushing one at a server must not have it
// surfaced there.
TEST_F(SessionConfigSim, ServerIgnoresSessionStatusSentByAClient)
{
	RakPeer *server = AddPooledServer(8, 4, 0);
	RakPeer *client = AddClient(60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, guid));

	BitStream forged;
	forged.Write((MessageID) ID_SESSION_CONFIG_STATUS);
	forged.Write("spoofed", 7);
	SendGateBypass::Inject(client, forged, net.AddressOf(server));

	Pump(1200);
	EXPECT_FALSE(Has(server, ID_SESSION_CONFIG_STATUS)) << "a client pushed a status into the server's queue";
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION));

	// The real handshake is unaffected.
	server->AcceptSession(guid, nullptr, 0);
	ASSERT_TRUE(WaitFor(server, ID_NEW_INCOMING_CONNECTION));
}

// A client that gives up while the server decides is reported to the server as abandoned, exactly once,
// and answering it afterwards is inert.
TEST_F(SessionConfigSim, ClientLeavingMidDecisionIsReportedAsAbandoned)
{
	RakPeer *server = AddPooledServer(8, 4, 0);
	RakPeer *client = AddManualClient("10.0.9.1", 60000, 60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));

	RakNetGUID guid;
	ASSERT_TRUE(WaitForRequest(server, guid));

	// Shutdown with a block duration sends the disconnection notification, so the server learns now
	// rather than after the 60s connection timeout.
	//
	// Under manual update Shutdown() flushes with a single update cycle, and ReliabilityLayer::Update
	// sends nothing at a timestamp it has already updated at (time <= lastUpdateTime). Let a millisecond
	// pass first -- net.Step() advances the clock without updating this fixture-driven client -- as it
	// always does on a real clock, or the flush is a no-op and the server learns only by timeout.
	net.Step(1);
	client->Shutdown(300);

	ASSERT_TRUE(WaitFor(server, ID_SESSION_CONFIG_ABANDONED, 1000)) << "the server was never told the held request went away";
	EXPECT_EQ(guid, Find(server, ID_SESSION_CONFIG_ABANDONED)->guid);

	server->AcceptSession(guid, nullptr, 0);
	Pump(1200);
	EXPECT_EQ(1u, Count(server, ID_SESSION_CONFIG_ABANDONED)) << "abandonment was reported twice";
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION)) << "accepting an abandoned request reported a connection";
	EXPECT_FALSE(Has(server, ID_CONNECTION_LOST)) << "a connection never reported was reported lost";
	EXPECT_FALSE(Has(server, ID_DISCONNECTION_NOTIFICATION)) << "a connection never reported was reported closed";
	EXPECT_EQ(0u, server->NumberOfConnections());
	EXPECT_FALSE(SlotInspector::FirstActiveSlot(server).sawSlot);
}

// =========================================================================================================
// Application messages that overtake the session reply (issue #96).
//
// The accepting side reports ID_NEW_INCOMING_CONNECTION as soon as it answers the session request, and
// an application commonly sends right then. The connecting side only reports its connection once that
// answer (ID_SESSION_CONFIG) arrives, so when the datagram carrying it is lost or delayed, what the
// application sent next arrives first. It has been acked, so it is never resent: the connecting side
// holds it and delivers it right after ID_CONNECTION_REQUEST_ACCEPTED, bounded by
// MAXIMUM_HELD_SESSION_MESSAGES and MAXIMUM_HELD_SESSION_BYTES.
// =========================================================================================================

namespace
{
const MessageID kReliableStreamId = ID_USER_PACKET_ENUM + 10;
const MessageID kUnreliableStreamId = ID_USER_PACKET_ENUM + 11;
const MessageID kOrderedStreamId = ID_USER_PACKET_ENUM + 12;
const MessageID kSequencedStreamId = ID_USER_PACKET_ENUM + 13;
const MessageID kStreamIds[] = {kReliableStreamId, kUnreliableStreamId, kOrderedStreamId, kSequencedStreamId};
// Not channel 0: messages ordered on channel 0 queue behind ID_SESSION_CONFIG in the reliability layer
// and were never affected.
const char kOrderedChannel = 3;
const char kSequencedChannel = 2;
const uint32_t kBurst = 5;

void WriteNumbered(BitStream &bs, MessageID id, uint32_t n)
{
	bs.Write(id);
	bs.Write(n);
}

// The numbers carried by every message with this id, in delivery order.
std::vector<uint32_t> NumbersIn(const std::vector<Message> &inbox, MessageID id)
{
	std::vector<uint32_t> numbers;
	for (const Message &message : inbox)
	{
		if (message.id != id || message.data.size() < 1 + sizeof(uint32_t))
			continue;
		BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
		bs.IgnoreBytes(1);
		uint32_t n = 0;
		bs.Read(n);
		numbers.push_back(n);
	}
	return numbers;
}

size_t FirstIndexOf(const std::vector<Message> &inbox, MessageID id)
{
	for (size_t i = 0; i < inbox.size(); ++i)
		if (inbox[i].id == id)
			return i;
	return inbox.size();
}

std::vector<uint32_t> Range(uint32_t first, uint32_t end)
{
	std::vector<uint32_t> out;
	for (uint32_t n = first; n < end; ++n)
		out.push_back(n);
	return out;
}

size_t CountUserMessages(const std::vector<Message> &inbox)
{
	size_t n = 0;
	for (const Message &message : inbox)
		n += message.id >= ID_USER_PACKET_ENUM ? 1 : 0;
	return n;
}
} // namespace

class SessionHeldMessagesSim : public SessionConfigSim
{
protected:
	enum class ReplyFate
	{
		Lost,   // dropped, so the reliability layer resends it a retransmission timeout later
		Delayed // overtaken by everything sent after it
	};

	void SpoilLink(const RakPeer *from, const RakPeer *to, ReplyFate fate)
	{
		SimulatedNetwork::LinkRule rule;
		if (fate == ReplyFate::Lost)
			rule.dark = true;
		else
			rule.delayMs = 400;
		net.SetLink(from, to, rule);
	}

	void RestoreLink(const RakPeer *from, const RakPeer *to) { net.SetLink(from, to, SimulatedNetwork::LinkRule()); }

	// The link from `accepting` is already spoiled and the session request answered (or about to be): wait
	// for `accepting` to report the connection, let the update cycle after the report send the session
	// reply into the spoiled link, then restore a clean link for what the application sends next.
	void LetTheReplyGoBad(const RakPeer *accepting, const RakPeer *connecting)
	{
		ASSERT_TRUE(WaitFor(accepting, ID_NEW_INCOMING_CONNECTION)) << "the accepting side never reported the connection";
		Pump(5);
		RestoreLink(accepting, connecting);
	}

	// What an application does on ID_NEW_INCOMING_CONNECTION: send at once, here on every reliability.
	void SendBurst(RakPeer *from, const RakNetGUID &to, uint32_t first, uint32_t count)
	{
		struct Stream
		{
			MessageID id;
			Reliability reliability;
			char channel;
		};
		const Stream streams[] = {
		    {kReliableStreamId, Reliability::Reliable, 0},
		    {kUnreliableStreamId, Reliability::Unreliable, 0},
		    {kOrderedStreamId, Reliability::ReliableOrdered, kOrderedChannel},
		    {kSequencedStreamId, Reliability::ReliableSequenced, kSequencedChannel},
		};
		for (uint32_t n = first; n < first + count; ++n)
		{
			for (const Stream &stream : streams)
			{
				BitStream bs;
				WriteNumbered(bs, stream.id, n);
				ASSERT_NE(0u, from->Send(&bs, Priority::High, stream.reliability, stream.channel, to, false));
			}
		}
	}

	// Pump until `connecting` has reported the connection and received every reliable message numbered
	// below `end`, recording the most messages it held at once.
	bool WaitForBurst(const RakPeer *connecting, const RakNetGUID &accepting, uint32_t end, unsigned int &peakHeld, uint32_t maxMs = 5000)
	{
		return Until([&]
		             {
			             peakHeld = std::max(peakHeld, SlotInspector::SlotFor(connecting, accepting).heldMessages);
			             return Has(connecting, ID_CONNECTION_REQUEST_ACCEPTED) &&
			                    NumbersIn(Inbox(connecting), kReliableStreamId).size() >= end &&
			                    NumbersIn(Inbox(connecting), kOrderedStreamId).size() >= end; }, maxMs);
	}

	// Every reliable message numbered below `end` arrived exactly once, after the connection was reported,
	// ordered ones in order; on the clean link the unordered and sequenced ones are all there too.
	void ExpectBurstDelivered(const RakPeer *connecting, uint32_t end)
	{
		const std::vector<Message> &inbox = Inbox(connecting);
		SCOPED_TRACE("inbox " + IdList(inbox));
		ASSERT_EQ(1u, Count(connecting, ID_CONNECTION_REQUEST_ACCEPTED));
		const size_t accepted = FirstIndexOf(inbox, ID_CONNECTION_REQUEST_ACCEPTED);
		for (const MessageID id : kStreamIds)
			EXPECT_GT(FirstIndexOf(inbox, id), accepted) << "message " << (int) id << " was delivered before its connection was reported";

		std::vector<uint32_t> reliable = NumbersIn(inbox, kReliableStreamId);
		std::sort(reliable.begin(), reliable.end());
		EXPECT_EQ(Range(0, end), reliable) << "Reliable messages lost or duplicated";
		EXPECT_EQ(Range(0, end), NumbersIn(inbox, kOrderedStreamId)) << "the ordered channel lost, skipped or reordered a message";
		EXPECT_EQ(Range(0, end), NumbersIn(inbox, kUnreliableStreamId)) << "Unreliable messages lost on a clean link";
		EXPECT_EQ(Range(0, end), NumbersIn(inbox, kSequencedStreamId)) << "ReliableSequenced messages lost on a clean link";
		EXPECT_FALSE(Has(connecting, ID_CONNECTION_ATTEMPT_FAILED));
	}

	// The whole scenario from the moment the session reply has gone bad: burst, wait, check, then a second
	// burst on the established connection continues every stream without a gap.
	void RunBurst(RakPeer *accepting, RakPeer *connecting)
	{
		const RakNetGUID acceptingGuid = accepting->GetMyGUID();
		const RakNetGUID connectingGuid = connecting->GetMyGUID();
		ASSERT_FALSE(Has(connecting, ID_CONNECTION_REQUEST_ACCEPTED)) << "precondition: the session reply must not have arrived yet";
		ASSERT_NO_FATAL_FAILURE(SendBurst(accepting, connectingGuid, 0, kBurst));

		unsigned int peakHeld = 0;
		ASSERT_TRUE(WaitForBurst(connecting, acceptingGuid, kBurst, peakHeld)) << "inbox " << IdList(Inbox(connecting));
		// The scenario only proves something if the burst really did overtake the session reply.
		EXPECT_GE(peakHeld, kBurst) << "the burst did not arrive inside the handshake window";
		ExpectBurstDelivered(connecting, kBurst);
		EXPECT_EQ(0u, SlotInspector::SlotFor(connecting, acceptingGuid).heldMessages);

		ASSERT_NO_FATAL_FAILURE(SendBurst(accepting, connectingGuid, kBurst, kBurst));
		ASSERT_TRUE(WaitForBurst(connecting, acceptingGuid, 2 * kBurst, peakHeld));
		Pump(50);
		ExpectBurstDelivered(connecting, 2 * kBurst);
	}

	// Static mode: the accepting side answers the request the moment it arrives, so the link goes bad as
	// soon as the connecting side is in the handshake.
	void RunStatic(ReplyFate fate)
	{
		RakPeer *server = AddServer(8, 8);
		RakPeer *client = AddClient();
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
		ASSERT_TRUE(Until([&]
		                  { return SlotInspector::SlotFor(client, server->GetMyGUID()).exchanging; }, kConnectTimeoutMs));
		SpoilLink(server, client, fate);
		ASSERT_NO_FATAL_FAILURE(LetTheReplyGoBad(server, client));
		RunBurst(server, client);
	}

	// Interactive mode: the reply goes out when the application calls AcceptSession().
	void RunInteractive(ReplyFate fate)
	{
		RakPeer *server = AddServer(8, 8);
		RakPeer *client = AddClient();
		server->SetSessionConfigInteractive(true);
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
		RakNetGUID clientGuid;
		ASSERT_TRUE(WaitForRequest(server, clientGuid));
		SpoilLink(server, client, fate);
		server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
		ASSERT_NO_FATAL_FAILURE(LetTheReplyGoBad(server, client));
		ASSERT_NO_FATAL_FAILURE(RunBurst(server, client));

		unsigned int length = 0;
		const char *payload = client->GetRemoteSessionConfig(server->GetMyGUID(), &length);
		ExpectPayload(payload, length, kServerPayload, std::strlen(kServerPayload));
	}

	// Interactive server, client parked in the handshake: what a non-conforming server pushes at it now
	// stays held for as long as the server takes to decide.
	void ParkClient(RakPeer *&server, RakPeer *&client, RakNetGUID &clientGuid, TimeMS timeoutMs = 60000)
	{
		server = AddServer(8, 8, timeoutMs);
		client = AddClient(timeoutMs);
		server->SetSessionConfigInteractive(true);
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
		ASSERT_TRUE(WaitForRequest(server, clientGuid));
	}

	void InjectNumbered(RakPeer *from, const RakPeer *to, MessageID id, uint32_t first, uint32_t count, Reliability reliability, char channel = 0)
	{
		for (uint32_t n = first; n < first + count; ++n)
		{
			BitStream bs;
			WriteNumbered(bs, id, n);
			SendGateBypass::Inject(from, bs, net.AddressOf(to), reliability, channel);
		}
	}

	// Size of one InjectNumbered message: the id byte and a uint32_t counter.
	static constexpr unsigned int kNumberedMessageBytes = 1 + sizeof(uint32_t);

	// `count` messages of exactly `bytes` bytes each (id included).
	void InjectLarge(RakPeer *from, const RakPeer *to, uint32_t count, unsigned int bytes)
	{
		std::vector<unsigned char> body(bytes - 1 - sizeof(uint32_t), 0xA5);
		for (uint32_t n = 0; n < count; ++n)
		{
			BitStream bs;
			WriteNumbered(bs, kReliableStreamId, n);
			bs.WriteAlignedBytes(body.data(), (unsigned int) body.size());
			ASSERT_EQ(bytes, bs.GetNumberOfBytesUsed());
			SendGateBypass::Inject(from, bs, net.AddressOf(to), Reliability::Reliable);
		}
	}

	// An ID_SESSION_CONFIG carrying `payloadBytes` bytes, as AcceptSession() or a static server sends it, but of
	// any size: a server built with a larger MAXIMUM_SESSION_CONFIG_SIZE than this binary sends one this binary
	// must refuse (AcceptSession clamps to the sender's own limit, not the receiver's).
	void InjectSessionConfig(RakPeer *from, const RakPeer *to, unsigned int payloadBytes, Reliability reliability = Reliability::ReliableOrdered)
	{
		std::vector<unsigned char> payload(payloadBytes, 0x5A);
		BitStream bs;
		bs.Write((MessageID) ID_SESSION_CONFIG);
		if (payloadBytes > 0)
			bs.WriteAlignedBytes(payload.data(), payloadBytes);
		SendGateBypass::Inject(from, bs, net.AddressOf(to), reliability);
	}

	// ID_CONNECTION_REQUEST_ACCEPTED: id | external address | system index | internal addresses | ping | pong.
	// Sent by `from` although `to` never sent it an ID_CONNECTION_REQUEST.
	void ForgeConnectionAccepted(RakPeer *from, const RakPeer *to)
	{
		BitStream forged;
		forged.Write((MessageID) ID_CONNECTION_REQUEST_ACCEPTED);
		forged.Write(net.AddressOf(from));
		forged.Write((SystemIndex) 0);
		for (unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++)
			forged.Write(UNASSIGNED_SYSTEM_ADDRESS);
		forged.Write((Time) 0);
		forged.Write((Time) 0);
		SendGateBypass::Inject(from, forged, net.AddressOf(to));
	}

	bool WaitForHeld(const RakPeer *client, const RakNetGUID &server, unsigned int count, uint32_t maxMs = 30000)
	{
		return Until([&]
		             { return SlotInspector::SlotFor(client, server).heldMessages >= count; }, maxMs);
	}

	// The connecting side broke off: told once, nothing delivered, slot freed; the accepting side never
	// reports a connection.
	void ExpectAttemptFailedWithNothingDelivered(const RakPeer *server, const RakPeer *client)
	{
		Pump(2000);
		SCOPED_TRACE("client inbox " + IdList(Inbox(client)));
		EXPECT_EQ(1u, Count(client, ID_CONNECTION_ATTEMPT_FAILED)) << "the attempt must be reported failed exactly once";
		EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED));
		EXPECT_EQ(0u, CountUserMessages(Inbox(client))) << "held messages reached the application of a connection that never completed";
		EXPECT_FALSE(SlotInspector::FirstActiveSlot(client).sawSlot) << "the client's slot was never released";
		EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION));
	}
};

TEST_F(SessionHeldMessagesSim, BurstOvertakingALostSessionReplyIsDeliveredAfterTheConnection)
{
	RunStatic(ReplyFate::Lost);
}

TEST_F(SessionHeldMessagesSim, BurstOvertakingADelayedSessionReplyIsDeliveredAfterTheConnection)
{
	RunStatic(ReplyFate::Delayed);
}

TEST_F(SessionHeldMessagesSim, InteractiveBurstOvertakingALostSessionReplyIsDeliveredAfterTheConnection)
{
	RunInteractive(ReplyFate::Lost);
}

TEST_F(SessionHeldMessagesSim, InteractiveBurstOvertakingADelayedSessionReplyIsDeliveredAfterTheConnection)
{
	RunInteractive(ReplyFate::Delayed);
}

// Both peers connect at once; the lower GUID's handshake wins, so the higher peer is the accepting side
// (its own Connect() absorbed) and reports ID_NEW_INCOMING_CONNECTION first.
TEST_F(SessionHeldMessagesSim, CrossConnectBurstOvertakingALostSessionReplyIsDeliveredAfterTheConnection)
{
	RakPeer *a = AddServer(1, 1);
	RakPeer *b = AddServer(1, 1);
	RakPeer *lower = a->GetMyGUID().g < b->GetMyGUID().g ? a : b;
	RakPeer *higher = lower == a ? b : a;

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(lower, higher));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(higher, lower));
	ASSERT_TRUE(Until([&]
	                  { return SlotInspector::SlotFor(lower, higher->GetMyGUID()).exchanging; }, kConnectTimeoutMs));
	SpoilLink(higher, lower, ReplyFate::Lost);
	ASSERT_NO_FATAL_FAILURE(LetTheReplyGoBad(higher, lower));
	ASSERT_NO_FATAL_FAILURE(RunBurst(higher, lower));

	EXPECT_EQ(0u, Count(higher, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(0u, Count(higher, ID_CONNECTION_ATTEMPT_FAILED)) << "the absorbed Connect() was reported failed";
	EXPECT_EQ(0u, Count(lower, ID_NEW_INCOMING_CONNECTION));

	// And the other way, from the connecting side, as on any connection.
	ASSERT_NO_FATAL_FAILURE(SendBurst(lower, higher->GetMyGUID(), 0, kBurst));
	ASSERT_TRUE(Until([&]
	                  { return NumbersIn(Inbox(higher), kOrderedStreamId).size() >= kBurst &&
		                       NumbersIn(Inbox(higher), kReliableStreamId).size() >= kBurst; }, 5000));
	EXPECT_EQ(Range(0, kBurst), NumbersIn(Inbox(higher), kOrderedStreamId));
}

// The count bound is inclusive: exactly MAXIMUM_HELD_SESSION_MESSAGES are held, then all delivered in order.
TEST_F(SessionHeldMessagesSim, MessageLimitIsInclusive)
{
	// Both limits are build-time overridable: test the count boundary only when the byte budget lets it be
	// reached first, or the test would hit (or pass on) the byte limit instead.
	if constexpr ((uint64_t) MAXIMUM_HELD_SESSION_MESSAGES * kNumberedMessageBytes > MAXIMUM_HELD_SESSION_BYTES)
		GTEST_SKIP() << "MAXIMUM_HELD_SESSION_BYTES is below the message-count boundary in this build";
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	const uint32_t limit = MAXIMUM_HELD_SESSION_MESSAGES;
	InjectNumbered(server, client, kOrderedStreamId, 0, limit, Reliability::ReliableOrdered, kOrderedChannel);
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), limit)) << "held " << SlotInspector::SlotFor(client, server->GetMyGUID()).heldMessages;
	EXPECT_FALSE(Has(client, ID_CONNECTION_ATTEMPT_FAILED));

	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(Until([&]
	                  { return NumbersIn(Inbox(client), kOrderedStreamId).size() >= limit; }, 5000));
	ASSERT_EQ(1u, Count(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_LT(FirstIndexOf(Inbox(client), ID_CONNECTION_REQUEST_ACCEPTED), FirstIndexOf(Inbox(client), kOrderedStreamId));
	EXPECT_EQ(Range(0, limit), NumbersIn(Inbox(client), kOrderedStreamId));
	EXPECT_EQ(0u, SlotInspector::SlotFor(client, server->GetMyGUID()).heldMessages);
}

// One message over the count bound fails the attempt instead of silently losing that message.
TEST_F(SessionHeldMessagesSim, ExceedingTheMessageLimitFailsTheAttempt)
{
	if constexpr ((uint64_t) (MAXIMUM_HELD_SESSION_MESSAGES + 1u) * kNumberedMessageBytes > MAXIMUM_HELD_SESSION_BYTES)
		GTEST_SKIP() << "MAXIMUM_HELD_SESSION_BYTES would overflow before the message count in this build";
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	// One over the limit, then a few more that arrive once the attempt has already failed.
	InjectNumbered(server, client, kReliableStreamId, 0, MAXIMUM_HELD_SESSION_MESSAGES + 10u, Reliability::Reliable);

	// Well inside the 60s handshake timeout: this failure can only be the limit.
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, 30000)) << "exceeding the held-message limit did not fail the attempt";
	// The connecting side told the server it is gone, so its pending request is abandoned now rather than
	// after the timeout.
	ASSERT_TRUE(WaitFor(server, ID_SESSION_CONFIG_ABANDONED, 2000)) << "the server was not told the client broke off";
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ExpectAttemptFailedWithNothingDelivered(server, client);
}

// The byte bound is inclusive too.
TEST_F(SessionHeldMessagesSim, ByteLimitIsInclusive)
{
	// The byte boundary uses 4 messages (5 to overflow): the count budget must allow that.
	if constexpr (MAXIMUM_HELD_SESSION_MESSAGES < 5u)
		GTEST_SKIP() << "MAXIMUM_HELD_SESSION_MESSAGES is below the byte-boundary scenario in this build";
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	const unsigned int messageBytes = MAXIMUM_HELD_SESSION_BYTES / 4;
	ASSERT_NO_FATAL_FAILURE(InjectLarge(server, client, 4, messageBytes));
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), 4));
	EXPECT_EQ((unsigned int) MAXIMUM_HELD_SESSION_BYTES, SlotInspector::SlotFor(client, server->GetMyGUID()).heldBytes);

	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(Until([&]
	                  { return Count(client, kReliableStreamId) >= 4; }, 5000));
	EXPECT_LT(FirstIndexOf(Inbox(client), ID_CONNECTION_REQUEST_ACCEPTED), FirstIndexOf(Inbox(client), kReliableStreamId));
	for (const Message &message : Inbox(client))
	{
		if (message.id == kReliableStreamId)
		{
			EXPECT_EQ(messageBytes, message.data.size());
		}
	}
	std::vector<uint32_t> numbers = NumbersIn(Inbox(client), kReliableStreamId);
	std::sort(numbers.begin(), numbers.end());
	EXPECT_EQ(Range(0, 4), numbers);
}

TEST_F(SessionHeldMessagesSim, ExceedingTheByteLimitFailsTheAttempt)
{
	if constexpr (MAXIMUM_HELD_SESSION_MESSAGES < 5u)
		GTEST_SKIP() << "MAXIMUM_HELD_SESSION_MESSAGES would overflow before the bytes in this build";
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	ASSERT_NO_FATAL_FAILURE(InjectLarge(server, client, 4, MAXIMUM_HELD_SESSION_BYTES / 4));
	ASSERT_NO_FATAL_FAILURE(InjectLarge(server, client, 1, 64));

	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, 30000)) << "exceeding the held-byte limit did not fail the attempt";
	ASSERT_TRUE(WaitFor(server, ID_SESSION_CONFIG_ABANDONED, 2000)) << "the server was not told the client broke off";
	ExpectAttemptFailedWithNothingDelivered(server, client);
}

// A refusal arriving behind held messages: none of them is ever delivered, and they are freed with the slot
// (which a LeakSanitizer run, ASAN_OPTIONS=detect_leaks=1 on Linux, checks).
TEST_F(SessionHeldMessagesSim, RejectedSessionNeverDeliversHeldMessages)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	InjectNumbered(server, client, kReliableStreamId, 0, 3, Reliability::Reliable);
	InjectNumbered(server, client, kOrderedStreamId, 0, 3, Reliability::ReliableOrdered, kOrderedChannel);
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), 6));

	const char reason[] = "server full";
	server->RejectSession(clientGuid, reason);
	// Whatever a non-conforming server sends behind its refusal (here in the same datagram) is for an
	// attempt already answered.
	InjectNumbered(server, client, kReliableStreamId, 3, 2, Reliability::Reliable);
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED));
	const Message *failed = Find(client, ID_CONNECTION_ATTEMPT_FAILED);
	ASSERT_EQ(1 + std::strlen(reason), failed->data.size()) << "the refusal's reason was lost";
	ExpectAttemptFailedWithNothingDelivered(server, client);
}

// A handshake that times out with messages held: the attempt fails once, nothing is delivered.
TEST_F(SessionHeldMessagesSim, TimedOutHandshakeNeverDeliversHeldMessages)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid, kHandshakeTimeoutMs));
	InjectNumbered(server, client, kReliableStreamId, 0, 3, Reliability::Reliable);
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), 3, 1000));

	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, kHandshakeTimeoutMs + 2000)) << "the unanswered handshake never timed out";
	ExpectAttemptFailedWithNothingDelivered(server, client);
}

namespace
{
// Records what a network-thread plugin (#88) sees, and whether the connection had been reported to the
// application by then.
class HeldMessageProbe : public PluginInterface2
{
public:
	// A wide id field on purpose: with a 2-byte struct, GCC 13 at -O2 reports a false
	// -Wstringop-overflow inside vector::push_back.
	struct Seen
	{
		unsigned int id;
		bool reported;
	};

	PluginReceiveResult OnReceiveNetworkThread(Packet *packet) override
	{
		if (packet->data[0] >= ID_USER_PACKET_ENUM)
			networkThread.push_back({(unsigned int) packet->data[0], SlotInspector::SlotFor(peer, packet->guid).reported});
		return RR_CONTINUE_PROCESSING;
	}

	PluginReceiveResult OnReceive(Packet *packet) override
	{
		userThread.push_back(packet->data[0]);
		return RR_CONTINUE_PROCESSING;
	}

	const RakPeer *peer = nullptr;
	std::vector<Seen> networkThread;
	std::vector<MessageID> userThread;
};
} // namespace

// Network-thread plugins see a held message when it is released, on a connection already reported, like any
// later message -- never when it arrived -- and user-thread plugins see it after the connection packet.
TEST_F(SessionHeldMessagesSim, PluginsSeeHeldMessagesWhenTheyAreReleased)
{
	RakPeer *server = AddServer(8, 8);
	RakPeer *client = AddClient();
	HeldMessageProbe probe;
	probe.peer = client;
	client->AttachPlugin(&probe);

	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(Until([&]
	                  { return SlotInspector::SlotFor(client, server->GetMyGUID()).exchanging; }, kConnectTimeoutMs));
	SpoilLink(server, client, ReplyFate::Lost);
	ASSERT_NO_FATAL_FAILURE(LetTheReplyGoBad(server, client));
	ASSERT_NO_FATAL_FAILURE(SendBurst(server, client->GetMyGUID(), 0, kBurst));

	unsigned int peakHeld = 0;
	ASSERT_TRUE(WaitForBurst(client, server->GetMyGUID(), kBurst, peakHeld));
	EXPECT_GE(peakHeld, kBurst) << "the burst did not arrive inside the handshake window";
	ExpectBurstDelivered(client, kBurst);

	// Every user message of the burst: four streams of kBurst, all on a clean link.
	ASSERT_EQ(4u * kBurst, probe.networkThread.size());
	for (const HeldMessageProbe::Seen &seen : probe.networkThread)
		EXPECT_TRUE(seen.reported) << "a network-thread plugin saw message " << (int) seen.id << " before the connection was reported";
	ASSERT_FALSE(probe.userThread.empty());
	EXPECT_EQ((MessageID) ID_CONNECTION_REQUEST_ACCEPTED, probe.userThread.front()) << "a user-thread plugin saw data before the connection packet";
	client->DetachPlugin(&probe);
}

// The application closes the connection attempt while messages are held: none is delivered, and the slot
// frees them (LeakSanitizer, detect_leaks=1 on Linux, checks that).
TEST_F(SessionHeldMessagesSim, CloseConnectionWhileHoldingDeliversNothing)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	InjectNumbered(server, client, kReliableStreamId, 0, 4, Reliability::Reliable);
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), 4));

	client->CloseConnection(net.AddressOf(server), true);
	ASSERT_TRUE(Until([&]
	                  { return !SlotInspector::FirstActiveSlot(client).sawSlot; }, 5000))
	    << "the client's slot was never released";
	Pump(500);
	EXPECT_EQ(0u, CountUserMessages(Inbox(client)));
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(Has(server, ID_NEW_INCOMING_CONNECTION));
}

// Shutdown() while messages are held frees them with the slot (LeakSanitizer checks) and delivers nothing.
TEST_F(SessionHeldMessagesSim, ShutdownWhileHoldingDeliversNothing)
{
	RakPeer *server = AddServer(8, 8, 60000);
	server->SetSessionConfigInteractive(true);
	RakPeer *client = AddManualClient("10.0.9.1", 60000, 60000);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	RakNetGUID clientGuid;
	ASSERT_TRUE(WaitForRequest(server, clientGuid));
	InjectNumbered(server, client, kReliableStreamId, 0, 4, Reliability::Reliable);
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), 4));

	client->Shutdown(0);
	EXPECT_FALSE(SlotInspector::FirstActiveSlot(client).sawSlot);
	EXPECT_EQ(0u, CountUserMessages(Inbox(client)));
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED));
}

// A peer that connected to us sends ID_CONNECTION_REQUEST_ACCEPTED itself while we are still handling its
// request (#99), then data. The accept is ignored, so our slot never takes the connecting role, and it must
// not make us hold that data either: only a connection we initiated has a session reply of ours to overtake.
// The hold keeps its own guard on weInitiatedTheConnection (#96); with the accept ignored that guard is
// defence in depth, so this test now fails on the role flip rather than on the hold.
TEST_F(SessionHeldMessagesSim, IncomingPeerCannotMakeTheAcceptingSideHold)
{
	RakPeer *server = AddServer(8, 8, 60000);
	RakPeer *client = AddClient(60000);
	// Slow the way back so the client is still waiting for our accept when it forges its own.
	SimulatedNetwork::LinkRule slow;
	slow.delayMs = 50;
	net.SetLink(server, client, slow);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(Until([&]
	                  { return SlotInspector::SlotFor(server, client->GetMyGUID()).handlingRequest; }, kConnectTimeoutMs));

	ForgeConnectionAccepted(client, server);
	InjectNumbered(client, server, kReliableStreamId, 0, 4, Reliability::Reliable);

	bool flipped = false;
	unsigned int peakHeld = 0;
	for (int i = 0; i < 300; ++i)
	{
		Pump(1);
		const SlotInspector::Snapshot slot = SlotInspector::SlotFor(server, client->GetMyGUID());
		flipped = flipped || slot.connectingSide;
		peakHeld = std::max(peakHeld, slot.heldMessages);
	}
	EXPECT_FALSE(flipped) << "a peer that connected to us switched our slot to the connecting role";
	EXPECT_EQ(0u, peakHeld) << "a peer that connected to us made us hold its data";
	EXPECT_EQ(0u, CountUserMessages(Inbox(server)));
}

// #99: the forged accept is ignored and the incoming connection completes as if it had never been sent. The
// server keeps the accepting role throughout, never reports ID_CONNECTION_REQUEST_ACCEPTED for a peer that
// connected to it, and reports ID_NEW_INCOMING_CONNECTION exactly once, with the client's payload; the
// client gets the server's. Ignoring rather than failing matches the other role-bound replies
// (ID_SESSION_CONFIG, ID_SESSION_CONFIG_REJECTED, ID_SESSION_CONFIG_STATUS): a message the peer has no
// business sending is dropped, and a peer that wants the connection gone can simply disconnect.
TEST_F(SessionHeldMessagesSim, ForgedAcceptFromAnIncomingPeerIsIgnored)
{
	RakPeer *server = AddServer(8, 8, 60000);
	RakPeer *client = AddClient(60000);
	server->SetSessionConfig(kServerPayload, (unsigned int) std::strlen(kServerPayload));
	client->SetSessionConfig(kClientPayload, (unsigned int) std::strlen(kClientPayload));
	SimulatedNetwork::LinkRule slow;
	slow.delayMs = 50;
	net.SetLink(server, client, slow);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(Until([&]
	                  { return SlotInspector::SlotFor(server, client->GetMyGUID()).handlingRequest; }, kConnectTimeoutMs));
	ForgeConnectionAccepted(client, server);

	bool flipped = false;
	ASSERT_TRUE(Until([&]
	                  {
		                  flipped = flipped || SlotInspector::SlotFor(server, client->GetMyGUID()).connectingSide;
		                  return Has(server, ID_NEW_INCOMING_CONNECTION) && Has(client, ID_CONNECTION_REQUEST_ACCEPTED); }, kConnectTimeoutMs))
	    << "client " << IdList(Inbox(client)) << " server " << IdList(Inbox(server));
	Pump(2000);

	SCOPED_TRACE("client " + IdList(Inbox(client)) + " server " + IdList(Inbox(server)));
	EXPECT_FALSE(flipped) << "the server took the connecting role for a peer that connected to it";
	EXPECT_EQ(0u, Count(server, ID_CONNECTION_REQUEST_ACCEPTED)) << "the server reported an accept it never asked for";
	EXPECT_EQ(1u, Count(server, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(1u, Count(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(0u, Count(server, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(0u, Count(client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(0u, Count(server, ID_CONNECTION_LOST));
	EXPECT_EQ(0u, Count(client, ID_CONNECTION_LOST));

	unsigned int length = 0;
	const char *fromClient = server->GetRemoteSessionConfig(client->GetMyGUID(), &length);
	ExpectPayload(fromClient, length, kClientPayload, std::strlen(kClientPayload));
	const char *fromServer = client->GetRemoteSessionConfig(server->GetMyGUID(), &length);
	ExpectPayload(fromServer, length, kServerPayload, std::strlen(kServerPayload));

	// And the connection carries data both ways.
	BitStream ping;
	WriteNumbered(ping, kReliableStreamId, 7);
	ASSERT_NE(0u, client->Send(&ping, Priority::High, Reliability::ReliableOrdered, 0, server->GetMyGUID(), false));
	ASSERT_NE(0u, server->Send(&ping, Priority::High, Reliability::ReliableOrdered, 0, client->GetMyGUID(), false));
	ASSERT_TRUE(Until([&]
	                  { return NumbersIn(Inbox(server), kReliableStreamId).size() == 1 && NumbersIn(Inbox(client), kReliableStreamId).size() == 1; }, 5000));
}

// #99 with AllowConnectionResponseIPMigration(true), which takes ID_CONNECTION_REQUEST_ACCEPTED in any state:
// on a connection the remote initiated it is still never ours to take, so a connected peer cannot restart
// the handshake on our side in the connecting role.
TEST_F(SessionHeldMessagesSim, ForgedAcceptOnAnEstablishedIncomingConnectionIsIgnoredWithIPMigration)
{
	RakPeer *server = AddServer(8, 8, 60000);
	RakPeer *client = AddClient(60000);
	server->AllowConnectionResponseIPMigration(true);
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, StartConnect(client, server));
	ASSERT_TRUE(WaitForBoth(server, ID_NEW_INCOMING_CONNECTION, client, ID_CONNECTION_REQUEST_ACCEPTED));

	ForgeConnectionAccepted(client, server);
	bool disturbed = false;
	for (int i = 0; i < 500; ++i)
	{
		Pump(1);
		const SlotInspector::Snapshot slot = SlotInspector::SlotFor(server, client->GetMyGUID());
		disturbed = disturbed || !slot.sawSlot || slot.exchanging || slot.connectingSide;
	}
	SCOPED_TRACE("client " + IdList(Inbox(client)) + " server " + IdList(Inbox(server)));
	EXPECT_FALSE(disturbed) << "a forged accept moved an established incoming connection back into the handshake";
	EXPECT_EQ(0u, Count(server, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_EQ(1u, Count(server, ID_NEW_INCOMING_CONNECTION));

	BitStream ping;
	WriteNumbered(ping, kReliableStreamId, 1);
	ASSERT_NE(0u, server->Send(&ping, Priority::High, Reliability::ReliableOrdered, 0, client->GetMyGUID(), false));
	ASSERT_TRUE(Until([&]
	                  { return NumbersIn(Inbox(client), kReliableStreamId).size() == 1; }, 5000));
}

// #98: the accepting side's ID_SESSION_CONFIG is larger than this binary's MAXIMUM_SESSION_CONFIG_SIZE (a
// server built with a larger limit, or a misbehaving one). That is a protocol violation, and the attempt
// fails like a refused one: ID_CONNECTION_ATTEMPT_FAILED exactly once, nothing held is delivered, the slot is
// freed. The server, still deciding here, is told the client broke off (ID_SESSION_CONFIG_ABANDONED) rather
// than waiting out the handshake timeout.
TEST_F(SessionHeldMessagesSim, OversizedSessionConfigFailsTheAttempt)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	InjectNumbered(server, client, kReliableStreamId, 0, 3, Reliability::Reliable);
	ASSERT_TRUE(WaitForHeld(client, server->GetMyGUID(), 3));

	// In place of AcceptSession(): the same message, one byte over this binary's limit.
	InjectSessionConfig(server, client, MAXIMUM_SESSION_CONFIG_SIZE + 1u);

	// Well inside the 60s handshake timeout: this failure can only be the oversized payload.
	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, 30000)) << "an oversized session config left Connect() unanswered";
	ASSERT_TRUE(WaitFor(server, ID_SESSION_CONFIG_ABANDONED, 2000)) << "the server was not told the client broke off";
	EXPECT_EQ(server->GetMyGUID(), Find(client, ID_CONNECTION_ATTEMPT_FAILED)->guid);
	ExpectAttemptFailedWithNothingDelivered(server, client);
}

// #98 as the issue describes it: the server has already reported the connection (AcceptSession() answered)
// when the oversized payload reaches the client. The client fails its attempt once and tells the server,
// which sees the connection end with ID_DISCONNECTION_NOTIFICATION instead of a timeout. The oversized
// message is ordered on channel 0 just ahead of the real reply, so the real one is processed after it, on a
// slot that is already closing, where it is ignored.
TEST_F(SessionHeldMessagesSim, OversizedSessionConfigAfterTheServerReportedTheConnection)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	InjectSessionConfig(server, client, MAXIMUM_SESSION_CONFIG_SIZE + 1u);
	Pump(1); // the injected message takes its place on channel 0 before the real reply
	server->AcceptSession(clientGuid, kServerPayload, (unsigned int) std::strlen(kServerPayload));
	ASSERT_TRUE(WaitFor(server, ID_NEW_INCOMING_CONNECTION, 2000)) << "precondition: the server must report the connection";

	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, 30000)) << "an oversized session config left Connect() unanswered";
	ASSERT_TRUE(WaitFor(server, ID_DISCONNECTION_NOTIFICATION, 5000)) << "the server was left to time the connection out";
	Pump(5000);
	SCOPED_TRACE("client " + IdList(Inbox(client)) + " server " + IdList(Inbox(server)));
	EXPECT_EQ(1u, Count(client, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_FALSE(Has(client, ID_CONNECTION_REQUEST_ACCEPTED)) << "the late real reply revived a failed attempt";
	EXPECT_FALSE(SlotInspector::FirstActiveSlot(client).sawSlot) << "the client's slot was never released";
	EXPECT_EQ(1u, Count(server, ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(1u, Count(server, ID_DISCONNECTION_NOTIFICATION));
	EXPECT_EQ(0u, Count(server, ID_CONNECTION_LOST));
	EXPECT_FALSE(SlotInspector::SlotFor(server, client->GetMyGUID()).sawSlot) << "the server's slot was never released";
}

// The boundary of #98: a payload of exactly MAXIMUM_SESSION_CONFIG_SIZE is accepted.
TEST_F(SessionHeldMessagesSim, SessionConfigAtTheLimitCompletesTheConnection)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	InjectSessionConfig(server, client, MAXIMUM_SESSION_CONFIG_SIZE);

	ASSERT_TRUE(WaitFor(client, ID_CONNECTION_REQUEST_ACCEPTED, 30000)) << "inbox " << IdList(Inbox(client));
	EXPECT_FALSE(Has(client, ID_CONNECTION_ATTEMPT_FAILED));
	unsigned int length = 0;
	ASSERT_NE(nullptr, client->GetRemoteSessionConfig(server->GetMyGUID(), &length));
	EXPECT_EQ((unsigned int) MAXIMUM_SESSION_CONFIG_SIZE, length);
}

// A failed attempt is terminal. The remote may close gracefully right behind whatever made the attempt fail,
// in the same delivery step; that ID_DISCONNECTION_NOTIFICATION must not report the close a second time.
class SessionTerminalFailureSim : public SessionHeldMessagesSim
{
protected:
	// What a server's graceful CloseConnection() sends, ordered on channel 0 behind everything injected before.
	void InjectDisconnect(RakPeer *from, const RakPeer *to)
	{
		BitStream bs;
		bs.Write((MessageID) ID_DISCONNECTION_NOTIFICATION);
		SendGateBypass::Inject(from, bs, net.AddressOf(to));
	}

	// The client's Connect() was answered exactly once, with ID_CONNECTION_ATTEMPT_FAILED, and nothing else
	// about that peer ever surfaced; its slot is gone.
	void ExpectFailedExactlyOnce(const RakPeer *client, const RakNetGUID &server)
	{
		ASSERT_TRUE(WaitFor(client, ID_CONNECTION_ATTEMPT_FAILED, 30000)) << "inbox " << IdList(Inbox(client));
		Pump(3000);
		SCOPED_TRACE("client inbox " + IdList(Inbox(client)));
		EXPECT_EQ(1u, Count(client, ID_CONNECTION_ATTEMPT_FAILED));
		EXPECT_EQ(server, Find(client, ID_CONNECTION_ATTEMPT_FAILED)->guid);
		EXPECT_EQ(0u, Count(client, ID_DISCONNECTION_NOTIFICATION)) << "a failed attempt was reported closed a second time";
		EXPECT_EQ(0u, Count(client, ID_CONNECTION_LOST));
		EXPECT_EQ(0u, Count(client, ID_CONNECTION_REQUEST_ACCEPTED));
		EXPECT_FALSE(SlotInspector::FirstActiveSlot(client).sawSlot) << "the client's slot was never released";
	}
};

// #98's path (FailSessionHandshakeAsConnectingSide): the oversized session reply and the server's graceful
// close are injected in the same update cycle, so they reach the client together, in that order.
TEST_F(SessionTerminalFailureSim, OversizedSessionConfigThenRemoteDisconnectReportsOnce)
{
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	InjectSessionConfig(server, client, MAXIMUM_SESSION_CONFIG_SIZE + 1u);
	InjectDisconnect(server, client);
	ExpectFailedExactlyOnce(client, server->GetMyGUID());
}

// The held-message bound, the other FailSessionHandshakeAsConnectingSide caller: the message that overflows
// it and the server's graceful close arrive together, ordered on channel 0 (which is still held: the server
// has not answered the session request, so nothing queues behind its reply).
TEST_F(SessionTerminalFailureSim, HeldByteLimitOverflowThenRemoteDisconnectReportsOnce)
{
	if constexpr (MAXIMUM_HELD_SESSION_MESSAGES < 5u)
		GTEST_SKIP() << "MAXIMUM_HELD_SESSION_MESSAGES would overflow before the bytes in this build";
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;
	RakNetGUID clientGuid;
	ASSERT_NO_FATAL_FAILURE(ParkClient(server, client, clientGuid));
	const unsigned int sizes[] = {MAXIMUM_HELD_SESSION_BYTES / 4, MAXIMUM_HELD_SESSION_BYTES / 4, MAXIMUM_HELD_SESSION_BYTES / 4, MAXIMUM_HELD_SESSION_BYTES / 4, 64};
	uint32_t n = 0;
	for (const unsigned int bytes : sizes)
	{
		std::vector<unsigned char> body(bytes - 1 - sizeof(uint32_t), 0xA5);
		BitStream bs;
		WriteNumbered(bs, kReliableStreamId, n++);
		bs.WriteAlignedBytes(body.data(), (unsigned int) body.size());
		SendGateBypass::Inject(server, bs, net.AddressOf(client));
	}
	InjectDisconnect(server, client);
	ExpectFailedExactlyOnce(client, server->GetMyGUID());
	EXPECT_EQ(0u, CountUserMessages(Inbox(client)));
}
