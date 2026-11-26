// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/client.h"
#include "mafianet/server.h"
#include "mafianet/session_crypto.h"
#include "SimulatedNetwork.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
struct Greeting
{
	uint32_t value = 0;
	template <class Archive>
	void serialize(Archive &ar) { ar & value; }
};
constexpr MessageID greetingId = ID_USER_PACKET_ENUM;

// Records every client event in arrival order.
struct ClientLog
{
	explicit ClientLog(Client &client)
	{
		client.on<ConnectionStatus>([this](const ConnectionStatus &event)
		                            { statuses.push_back(event.update); });
		client.on<ConnectionAccepted>([this, &client](const ConnectionAccepted &event)
		                              {
			EXPECT_TRUE(client.connected());
			accepted.push_back(event); });
		client.on<ConnectionFailed>([this, &client](const ConnectionFailed &event)
		                            {
			EXPECT_FALSE(client.connected());
			failed.push_back(event); });
		client.on<ServerDisconnected>([this, &client](const ServerDisconnected &event)
		                              {
			EXPECT_FALSE(client.connected());
			disconnected.push_back(event); });
	}
	bool ended() const { return !failed.empty() || !disconnected.empty(); }
	std::vector<std::string> statuses;
	std::vector<ConnectionAccepted> accepted;
	std::vector<ConnectionFailed> failed;
	std::vector<ServerDisconnected> disconnected;
};

RakPeer *Host(Server &server)
{
	return static_cast<RakPeer *>(&server.transport());
}

// Poll a client whose transport refuses it until the attempt ends.
ConnectionFailure FailureOf(SimulatedNetwork &net, Client &client)
{
	ClientLog log(client);
	EXPECT_TRUE(net.RunUntil([&]
	                         { client.poll(); return log.ended(); }, 10000));
	EXPECT_TRUE(log.accepted.empty());
	EXPECT_EQ(log.failed.size(), 1u);
	return log.failed.empty() ? ConnectionFailure::NoResponse : log.failed.front().reason;
}
}

TEST(PlayerClient, AcceptedSessionCarriesPayloadStatusResponseAndTypedMessages)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	std::vector<ConnectionRequest> requests;
	std::vector<Player> players;
	std::vector<uint32_t> serverReceived;
	server.on<ConnectionRequested>([&](const ConnectionRequest &request)
	                               { requests.push_back(request); });
	server.on<ClientConnected>([](ClientConnection connection)
	                           { EXPECT_TRUE(connection.accept_player()); });
	server.on<PlayerJoined>([&](Player player)
	                        { players.push_back(player); });
	server.messages().on<Greeting>(greetingId, [&](const Greeting &greeting, const Sender &)
	                               { serverReceived.push_back(greeting.value); });

	auto &client = net.AddClient(Host(server), "ticket");
	ClientLog log(client);
	std::vector<uint32_t> clientReceived;
	client.messages().on<Greeting>(greetingId, [&](const Greeting &greeting, const Sender &)
	                               { clientReceived.push_back(greeting.value); });
	EXPECT_FALSE(client.connected());
	EXPECT_EQ(client.send(Greeting{1}), 0u);
	EXPECT_FALSE(client.disconnect());

	auto pump = [&]
	{
		server.poll();
		client.poll();
	};
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !requests.empty(); }, 5000));
	EXPECT_EQ(requests.front().payload(), "ticket");
	ASSERT_TRUE(requests.front().status("checking"));
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !log.statuses.empty(); }, 5000));
	EXPECT_EQ(log.statuses, std::vector<std::string>{"checking"});
	EXPECT_FALSE(client.connected());

	ASSERT_EQ(requests.front().accept(ConnectionSlot::Public, "world-7"), SessionAcceptResult::Accepted);
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !log.accepted.empty() && !players.empty(); }, 5000));
	ASSERT_EQ(log.accepted.size(), 1u);
	EXPECT_EQ(log.accepted.front().response, "world-7");
	EXPECT_TRUE(client.connected());

	EXPECT_NE(client.send(Greeting{11}), 0u);
	EXPECT_NE(players.front().send(Greeting{22}), 0u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !serverReceived.empty() && !clientReceived.empty(); }, 5000));
	EXPECT_EQ(serverReceived, std::vector<uint32_t>{11});
	EXPECT_EQ(clientReceived, std::vector<uint32_t>{22});
	EXPECT_TRUE(client.ping());
	EXPECT_TRUE(log.failed.empty());
	EXPECT_TRUE(log.disconnected.empty());
}

TEST(PlayerClient, RejectionDeliversTheServersCodeAndDetail)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({4});
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               {
		if (request.payload() == "banned")
			EXPECT_TRUE(request.reject(7, "Banned until Friday"));
		else if (request.payload() == "unknown")
			EXPECT_TRUE(request.reject("Unknown ticket"));
		else
			EXPECT_TRUE(request.reject()); });
	server.on<ClientConnected>([](ClientConnection)
	                           { ADD_FAILURE() << "A refused session connected"; });
	auto &banned = net.AddClient(Host(server), "banned");
	auto &unknown = net.AddClient(Host(server), "unknown");
	auto &silent = net.AddClient(Host(server), "silent");
	ClientLog bannedLog(banned), unknownLog(unknown), silentLog(silent);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		server.poll();
		banned.poll();
		unknown.poll();
		silent.poll();
		return bannedLog.ended() && unknownLog.ended() && silentLog.ended(); }, 5000));
	ASSERT_EQ(bannedLog.failed.size(), 1u);
	EXPECT_EQ(bannedLog.failed[0].reason, ConnectionFailure::Rejected);
	EXPECT_EQ(bannedLog.failed[0].code, 7u);
	EXPECT_EQ(bannedLog.failed[0].detail, "Banned until Friday");
	ASSERT_EQ(unknownLog.failed.size(), 1u);
	EXPECT_EQ(unknownLog.failed[0].reason, ConnectionFailure::Rejected);
	EXPECT_EQ(unknownLog.failed[0].code, 0u);
	EXPECT_EQ(unknownLog.failed[0].detail, "Unknown ticket");
	// A refusal without a reason is still a refusal, not an unanswered attempt.
	ASSERT_EQ(silentLog.failed.size(), 1u);
	EXPECT_EQ(silentLog.failed[0].reason, ConnectionFailure::Rejected);
	EXPECT_EQ(silentLog.failed[0].code, 0u);
	EXPECT_EQ(silentLog.failed[0].detail, "");
}

TEST(PlayerClient, UnansweredRequestReportsNoResponse)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	Host(server)->SetSessionTimeout(100);
	auto &client = net.AddClient(Host(server));
	client.transport().SetSessionTimeout(250);
	ClientLog log(client);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		server.poll();
		client.poll();
		return log.ended(); }, 5000));
	ASSERT_EQ(log.failed.size(), 1u);
	EXPECT_EQ(log.failed[0].reason, ConnectionFailure::NoResponse);
	EXPECT_TRUE(log.failed[0].detail.empty());
}

TEST(PlayerClient, ClosedWaitingPoolReportsServerFull)
{
	SimulatedNetwork net;
	auto *host = net.AddPeer(2, 2);
	host->SetMaximumPendingSessions(0, 0);
	EXPECT_EQ(FailureOf(net, net.AddClient(host)), ConnectionFailure::ServerFull);
}

TEST(PlayerClient, BannedAddressReportsBanned)
{
	SimulatedNetwork net;
	auto *host = net.AddPeer(2, 2);
	auto &client = net.AddClient(host);
	char address[64];
	net.AddressOf(static_cast<RakPeer *>(&client.transport())).ToString(false, address, sizeof(address));
	host->AddToBanList(address);
	EXPECT_EQ(FailureOf(net, client), ConnectionFailure::Banned);
}

TEST(PlayerClient, WrongPasswordReportsInvalidPassword)
{
	SimulatedNetwork net;
	auto *host = net.AddPeer(2, 2);
	host->SetIncomingPassword("secret", 6);
	EXPECT_EQ(FailureOf(net, net.AddClient(host)), ConnectionFailure::InvalidPassword);
}

TEST(PlayerClient, EncryptionModeMismatchReportsEncryptionMismatch)
{
	SimulatedNetwork net;
	auto *host = net.AddPeer(2, 2);
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	EXPECT_EQ(FailureOf(net, net.AddClient(host)), ConnectionFailure::EncryptionMismatch);
}

TEST(PlayerClient, PinnedIdentityRejectsAnotherServerAndAcceptsTheRightOne)
{
	SimulatedNetwork net;
	uint8_t publicKey[kIdentityPublicKeyBytes], secretKey[kIdentitySecretKeyBytes];
	uint8_t otherPublic[kIdentityPublicKeyBytes], otherSecret[kIdentitySecretKeyBytes];
	GenerateIdentityKeyPair(publicKey, secretKey);
	GenerateIdentityKeyPair(otherPublic, otherSecret);
	net.SetNewPeerIdentityKey(secretKey);
	auto &server = net.AddServer({2});
	net.SetNewPeerIdentityKey(nullptr);
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });

	PublicKey wrong;
	std::memcpy(wrong.serverIdentity, otherPublic, sizeof(wrong.serverIdentity));
	auto &wrongPin = net.AddClient(Host(server), {}, &wrong);
	ClientLog wrongLog(wrongPin);
	PublicKey right;
	std::memcpy(right.serverIdentity, publicKey, sizeof(right.serverIdentity));
	auto &pinned = net.AddClient(Host(server), {}, &right);
	ClientLog pinnedLog(pinned);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		server.poll();
		wrongPin.poll();
		pinned.poll();
		return wrongLog.ended() && !pinnedLog.accepted.empty(); }, 10000));
	ASSERT_EQ(wrongLog.failed.size(), 1u);
	EXPECT_EQ(wrongLog.failed[0].reason, ConnectionFailure::ServerIdentityMismatch);
	EXPECT_TRUE(pinned.connected());
}

TEST(PlayerClient, ServerKickReachesTheClientAndIsReportedAsClosedOnTheServer)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	std::vector<Player> players;
	std::vector<PlayerLeft> left;
	std::vector<ClientDisconnected> gone;
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	server.on<ClientConnected>([](ClientConnection connection)
	                           { EXPECT_TRUE(connection.accept_player()); });
	server.on<PlayerJoined>([&](Player player)
	                        { players.push_back(player); });
	server.on<PlayerLeft>([&](const PlayerLeft &event)
	                      { left.push_back(event); });
	server.on<ClientDisconnected>([&](const ClientDisconnected &event)
	                              { gone.push_back(event); });
	auto &client = net.AddClient(Host(server));
	ClientLog log(client);
	auto pump = [&]
	{
		server.poll();
		client.poll();
	};
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !players.empty() && client.connected(); }, 5000));
	ASSERT_TRUE(players.front().disconnect(42, "Idle for too long"));
	EXPECT_FALSE(players.front().disconnect(43, "again"));
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !log.disconnected.empty() && !gone.empty(); }, 5000));
	EXPECT_EQ(log.disconnected[0].reason, LeaveReason::Disconnected);
	EXPECT_EQ(log.disconnected[0].code, 42u);
	EXPECT_EQ(log.disconnected[0].detail, "Idle for too long");
	ASSERT_EQ(left.size(), 1u);
	EXPECT_EQ(left[0].reason, LeaveReason::Closed);
	EXPECT_EQ(left[0].code, 42u);
	EXPECT_EQ(left[0].detail, "Idle for too long");
	ASSERT_EQ(gone.size(), 1u);
	EXPECT_EQ(gone[0].reason, LeaveReason::Closed);
	EXPECT_EQ(gone[0].code, 42u);
	EXPECT_EQ(gone[0].detail, "Idle for too long");
	EXPECT_EQ(client.send(Greeting{1}), 0u);
	net.Step(500);
	pump();
	EXPECT_EQ(log.disconnected.size(), 1u);
}

TEST(PlayerClient, ClientDisconnectReachesTheServerAndIsReportedAsClosedOnTheClient)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	std::vector<PlayerLeft> left;
	std::vector<ClientDisconnected> gone;
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	server.on<ClientConnected>([](ClientConnection connection)
	                           { EXPECT_TRUE(connection.accept_player()); });
	server.on<PlayerLeft>([&](const PlayerLeft &event)
	                      { left.push_back(event); });
	server.on<ClientDisconnected>([&](const ClientDisconnected &event)
	                              { gone.push_back(event); });
	auto &client = net.AddClient(Host(server));
	ClientLog log(client);
	auto pump = [&]
	{
		server.poll();
		client.poll();
	};
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !server.players().empty() && client.connected(); }, 5000));
	// A detail that starts with a zero byte still round-trips with a zero code.
	const std::string detail("\0quit", 5);
	ASSERT_TRUE(client.disconnect(0, detail));
	EXPECT_FALSE(client.connected());
	EXPECT_FALSE(client.disconnect(3, "again"));
	EXPECT_EQ(client.send(Greeting{1}), 0u);
	ASSERT_TRUE(net.RunUntil([&]
	                         { pump(); return !log.disconnected.empty() && !gone.empty(); }, 5000));
	EXPECT_EQ(log.disconnected[0].reason, LeaveReason::Closed);
	EXPECT_EQ(log.disconnected[0].code, 0u);
	EXPECT_EQ(log.disconnected[0].detail, detail);
	ASSERT_EQ(left.size(), 1u);
	EXPECT_EQ(left[0].reason, LeaveReason::Disconnected);
	EXPECT_EQ(left[0].code, 0u);
	EXPECT_EQ(left[0].detail, detail);
	ASSERT_EQ(gone.size(), 1u);
	EXPECT_EQ(gone[0].detail, detail);
}

TEST(PlayerClient, ClientCodeReachesTheServer)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	std::vector<ClientDisconnected> gone;
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	server.on<ClientDisconnected>([&](const ClientDisconnected &event)
	                              { gone.push_back(event); });
	auto &client = net.AddClient(Host(server));
	ClientLog log(client);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		server.poll();
		client.poll();
		return client.connected(); }, 5000));
	ASSERT_TRUE(client.disconnect(9, "Switching servers"));
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		server.poll();
		client.poll();
		return !gone.empty() && !log.disconnected.empty(); }, 5000));
	EXPECT_EQ(gone[0].reason, LeaveReason::Disconnected);
	EXPECT_EQ(gone[0].code, 9u);
	EXPECT_EQ(gone[0].detail, "Switching servers");
	EXPECT_EQ(log.disconnected[0].code, 9u);
	EXPECT_EQ(log.disconnected[0].detail, "Switching servers");
}

TEST(PlayerClient, RawServerReasonsArriveAsDetailWithoutACode)
{
	SimulatedNetwork net;
	net.onSessionRequest = [](RakPeer &peer, const Packet &request)
	{
		const std::string payload(reinterpret_cast<const char *>(request.data + 1), request.length - 1);
		if (payload == "refuse")
			peer.RejectSession(request.guid, "Raw refusal");
		else
			peer.AcceptSession(request.guid, "raw", 3);
	};
	auto *host = net.AddPeer(2, 2);
	auto &refused = net.AddClient(host, "refuse");
	auto &accepted = net.AddClient(host, "join");
	ClientLog refusedLog(refused), acceptedLog(accepted);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		refused.poll();
		accepted.poll();
		return refusedLog.ended() && !acceptedLog.accepted.empty(); }, 5000));
	ASSERT_EQ(refusedLog.failed.size(), 1u);
	EXPECT_EQ(refusedLog.failed[0].reason, ConnectionFailure::Rejected);
	EXPECT_EQ(refusedLog.failed[0].code, 0u);
	EXPECT_EQ(refusedLog.failed[0].detail, "Raw refusal");
	EXPECT_EQ(acceptedLog.accepted[0].response, "raw");

	BitStream reason;
	reason.Write("Raw close", 9);
	host->CloseConnection(net.AddressOf(static_cast<RakPeer *>(&accepted.transport())), true, 0, Priority::Low, &reason);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		accepted.poll();
		return acceptedLog.ended(); }, 5000));
	EXPECT_EQ(acceptedLog.disconnected[0].reason, LeaveReason::Disconnected);
	EXPECT_EQ(acceptedLog.disconnected[0].code, 0u);
	EXPECT_EQ(acceptedLog.disconnected[0].detail, "Raw close");
}

TEST(PlayerClient, RawClientSeesACodelessReasonUnchanged)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({2});
	std::vector<ClientConnection> connections;
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	server.on<ClientConnected>([&](ClientConnection connection)
	                           { connections.push_back(connection); });
	auto *raw = net.AddPeer(1);
	ASSERT_EQ(net.StartConnect(raw, Host(server)), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return !connections.empty(); }, 5000));
	ASSERT_TRUE(connections[0].disconnect(0, "Server restarting"));
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return net.Received(raw, ID_DISCONNECTION_NOTIFICATION); }, 5000));
	const auto &data = net.Find(raw, ID_DISCONNECTION_NOTIFICATION)->data;
	EXPECT_EQ(std::string(data.begin() + 1, data.end()), "Server restarting");
}
