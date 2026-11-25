// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include "SimulatedNetwork.h"
#include <gtest/gtest.h>

using namespace MafiaNet;

namespace
{
// Move a real command out of the simulated network thread's queue and restore it after reconnect.
struct CommandQueueAccess : RakPeer
{
	static auto &queue(RakPeer &peer) { return peer.*&CommandQueueAccess::bufferedCommands; }
};
}

TEST(PlayerServer, SimulatedRestartLeavesServerOwnedPeerRunning)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	auto *host = static_cast<RakPeer *>(&server.transport());
	EXPECT_EQ(net.RestartPeer(host, 1), nullptr);
	EXPECT_TRUE(host->IsActive());
}

TEST(PlayerServer, OldLoginAndPlayerCannotTargetTheSameGuidAfterReconnect)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	std::vector<ClientConnection> connections;
	std::vector<Player> joined;
	std::vector<PlayerLeft> left;
	server.on<ClientConnected>([&](ClientConnection connection)
	                           { connections.push_back(connection); });
	server.on<PlayerJoined>([&](Player player)
	                        { joined.push_back(player); });
	server.on<PlayerLeft>([&](const PlayerLeft &event)
	                      { left.push_back(event); });

	auto connect = [&]
	{
		net.Inbox(client).clear();
		return net.StartConnect(client, host) == CONNECTION_ATTEMPT_STARTED &&
		       net.RunUntil([&]
		                    { server.poll(); return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED); }, 5000);
	};
	ASSERT_TRUE(connect());
	server.poll();
	ASSERT_EQ(connections.size(), 1u);
	ASSERT_TRUE(joined.empty());
	ASSERT_TRUE(connections[0].accept_player());
	ASSERT_TRUE(connections[0].accept_player());
	server.poll();
	ASSERT_EQ(joined.size(), 1u);
	ASSERT_EQ(joined[0].id(), PlayerId{1});
	ASSERT_TRUE(joined[0].disconnect());
	ASSERT_TRUE(net.RunUntilReceived(client, ID_DISCONNECTION_NOTIFICATION, 5000));
	ASSERT_TRUE(connect());
	// The saved handles belong to the first connection.
	EXPECT_FALSE(connections[0].accept_player());
	EXPECT_FALSE(joined[0].disconnect());
	EXPECT_FALSE(joined[0].ping());
	server.poll();
	ASSERT_EQ(connections.size(), 2u);
	ASSERT_EQ(left.size(), 1u);
	EXPECT_EQ(left.front().player_id, PlayerId{1});
	ASSERT_TRUE(connections[1].accept_player());
	server.poll();
	ASSERT_EQ(joined.size(), 2u);
	EXPECT_EQ(joined[1].id(), PlayerId{2});
	EXPECT_FALSE(joined[0].disconnect());
}

TEST(PlayerServer, MissingRequestHandlerLeavesSessionPendingUntilTimeout)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	host->SetSessionTimeout(100);
	client->SetSessionTimeout(250);
	std::vector<PeerGuid> abandoned;
	server.on<ConnectionAbandoned>([&](const ConnectionAbandoned &event)
	                               { abandoned.push_back(event.connection.guid); });
	server.on<ClientConnected>([](ClientConnection)
	                           { ADD_FAILURE() << "Unanswered session connected"; });
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         {
		server.poll();
		return !abandoned.empty() && net.Received(client, ID_CONNECTION_ATTEMPT_FAILED); }, 5000));
	EXPECT_EQ(abandoned, (std::vector<PeerGuid>{ToPeerGuid(client->GetMyGUID())}));
	EXPECT_TRUE(server.players().empty());
}

TEST(PlayerServer, RequestCanDeferAndAbandonWithoutAllocatingAPlayer)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	server.on<ClientConnected>([](ClientConnection client)
	                           { EXPECT_TRUE(client.accept_player()); });
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	std::vector<ConnectionRequest> requests;
	std::vector<PeerGuid> abandoned;
	std::vector<PlayerId> joined;
	server.on<ConnectionRequested>([&](const ConnectionRequest &request)
	                               { requests.push_back(request); });
	server.on<ConnectionAbandoned>([&](const ConnectionAbandoned &event)
	                               { abandoned.push_back(event.connection.guid); });
	server.on<PlayerJoined>([&](Player player)
	                        { joined.push_back(player.id()); });
	client->SetSessionConfig("login", 5);
	client->SetSessionTimeout(500);
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return !requests.empty(); }, 5000));
	EXPECT_EQ(requests[0].payload(), "login");
	EXPECT_TRUE(requests[0].status("waiting"));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_SESSION_CONFIG_STATUS, 5000));
	ASSERT_EQ(requests[0].accept(), SessionAcceptResult::Accepted);
	auto &queue = CommandQueueAccess::queue(*host);
	auto delayedDecision = queue.TryPop();
	ASSERT_TRUE(delayedDecision);
	host->SetSessionTimeout(100);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return !abandoned.empty(); }, 5000));
	EXPECT_TRUE(joined.empty());
	EXPECT_TRUE(server.pending_requests().empty());
	EXPECT_EQ(requests[0].accept(), SessionAcceptResult::Expired);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_ATTEMPT_FAILED, 5000));
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return requests.size() == 2; }, 5000));
	queue.Push(std::move(*delayedDecision));
	net.Step(10);
	EXPECT_FALSE(net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED));
	EXPECT_FALSE(requests[0].reject("obsolete"));
	EXPECT_EQ(requests[1].accept(), SessionAcceptResult::Accepted);
	EXPECT_FALSE(requests[1].reject("already accepted"));
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return !joined.empty(); }, 5000));
	EXPECT_EQ(joined.front(), PlayerId{1});
}

namespace
{
struct Greeting
{
	uint32_t value = 0;
	template <class Archive>
	void serialize(Archive &ar) { ar & value; }
};
constexpr MessageID greetingId = ID_USER_PACKET_ENUM;

}

TEST(PlayerServer, QueuedSendAndKickAreDroppedAtExecutionAfterReconnect)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	server.on<ClientConnected>([](ClientConnection client)
	                           { EXPECT_TRUE(client.accept_player()); });
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	std::vector<Player> players;
	server.on<PlayerJoined>([&](Player p)
	                        { players.push_back(p); });
	server.messages().register_message<Greeting>(greetingId);
	auto connect = [&]
	{
		net.Inbox(client).clear();
		return net.StartConnect(client, host) == CONNECTION_ATTEMPT_STARTED &&
		       net.RunUntil([&]
		                    { server.poll(); return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED); }, 5000);
	};
	ASSERT_TRUE(connect());
	server.poll();
	ASSERT_EQ(players.size(), 1u);
	ASSERT_TRUE(players[0].send(Greeting{99}));
	auto &queue = CommandQueueAccess::queue(*host);
	auto delayedSend = queue.TryPop();
	ASSERT_TRUE(delayedSend);
	ASSERT_TRUE(players[0].disconnect());
	auto delayedKick = queue.TryPop();
	ASSERT_TRUE(delayedKick);

	client->CloseConnection(net.AddressOf(host), true);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_DISCONNECTION_NOTIFICATION, 5000));
	ASSERT_TRUE(connect());
	server.poll();
	ASSERT_EQ(players.size(), 2u);
	queue.Push(std::move(*delayedSend));
	queue.Push(std::move(*delayedKick));
	ASSERT_TRUE(players[1].send(Greeting{42}));
	ASSERT_TRUE(net.RunUntilReceived(client, greetingId, 5000));
	net.Step(300);
	const auto *message = net.Find(client, greetingId);
	ASSERT_NE(message, nullptr);
	BitStream body(const_cast<unsigned char *>(message->data.data()), static_cast<unsigned int>(message->data.size()), false);
	body.IgnoreBytes(1);
	uint32_t received = 0;
	ASSERT_TRUE(body.Read(received));
	EXPECT_EQ(received, 42u);
	EXPECT_FALSE(net.Received(client, ID_DISCONNECTION_NOTIFICATION));
	EXPECT_FALSE(server.player(PlayerId{1}));
	EXPECT_TRUE(server.player(PlayerId{2}));
}

TEST(PlayerServer, PollDispatchesLoginAndSnapshotsAnUnjoinedDisconnect)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	std::vector<std::string> events;
	server.on<ClientConnected>([&](ClientConnection)
	                           { events.emplace_back("connected"); });
	server.on<PlayerJoined>([&](Player)
	                        { events.emplace_back("joined"); });
	server.on<PlayerLeft>([&](const PlayerLeft &)
	                      { events.emplace_back("left"); });
	server.on<ClientDisconnected>([&](const ClientDisconnected &e)
	                              {
		EXPECT_EQ(e.connection.guid, ToPeerGuid(client->GetMyGUID()));
		EXPECT_EQ(e.detail, "cancelled");
		events.emplace_back("disconnected"); });
	server.messages().on<Greeting>(greetingId, [&](const Greeting &g, const Sender &from)
	                               {
		EXPECT_EQ(g.value, 7u);
		EXPECT_TRUE(server.connection(from));
		events.emplace_back("login"); });
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED); }, 5000));
	EXPECT_EQ(events, (std::vector<std::string>{"connected"}));
	BitStream login;
	server.messages().encode(login, Greeting{7});
	client->Send(&login, Priority::High, Reliability::ReliableOrdered, 0, net.AddressOf(host), false);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return events.size() == 2; }, 5000));
	BitStream detail;
	detail.Write("cancelled", 9);
	client->CloseConnection(net.AddressOf(host), true, 0, Priority::High, &detail);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return events.size() == 3; }, 5000));
	EXPECT_EQ(events, (std::vector<std::string>{"connected", "login", "disconnected"}));
}

TEST(PlayerServer, HandlesExpireWhenServerIsDestroyed)
{
	std::optional<Player> saved;
	{
		SimulatedNetwork net;
		auto &server = net.AddServer({1});
		server.on<ClientConnected>([](ClientConnection client)
		                           { EXPECT_TRUE(client.accept_player()); });
		server.on<ConnectionRequested>([](const ConnectionRequest &request)
		                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
		auto *host = static_cast<RakPeer *>(&server.transport());
		auto *client = net.AddPeer(1);
		server.on<PlayerJoined>([&](Player player)
		                        { saved = player; });
		ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
		ASSERT_TRUE(net.RunUntil([&]
		                         { server.poll(); return saved.has_value(); }, 5000));
	}
	EXPECT_FALSE(saved->ping());
	EXPECT_FALSE(saved->disconnect());
	EXPECT_FALSE(saved->send(Greeting{1}));
}

TEST(PlayerServer, DisconnectInConnectedHandlerLeavesPlayerRosterEmpty)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	server.on<ClientConnected>([](ClientConnection connection)
	                           { EXPECT_TRUE(connection.disconnect()); });
	server.on<PlayerJoined>([](Player)
	                        { ADD_FAILURE() << "A closing client became a player"; });
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return net.Received(client, ID_DISCONNECTION_NOTIFICATION); }, 5000));
}

TEST(PlayerServer, PlayerListContainsOnlyAcceptedSessionsInNumberOrder)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({3});
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	auto *host = static_cast<RakPeer *>(&server.transport());
	std::vector<ClientConnection> clients;
	server.on<ClientConnected>([&](ClientConnection client)
	                           { clients.push_back(client); });
	for (unsigned i = 0; i < 3; ++i)
	{
		ASSERT_EQ(net.StartConnect(net.AddPeer(1), host), CONNECTION_ATTEMPT_STARTED);
		ASSERT_TRUE(net.RunUntil([&]
		                         { server.poll(); return clients.size() == i + 1; }, 5000));
	}
	ASSERT_EQ(clients[2].accept_player(), PlayerId{1});
	ASSERT_EQ(clients[0].accept_player(), PlayerId{2});
	const auto roster = server.players();
	ASSERT_EQ(roster.size(), 2u);
	EXPECT_EQ(roster[0].key(), clients[2].key());
	EXPECT_EQ(roster[1].key(), clients[0].key());
	ASSERT_TRUE(roster[0].disconnect());
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return server.players().size() == 1; }, 5000));
	EXPECT_FALSE(roster[0].disconnect());
	EXPECT_EQ(server.players().front().id(), PlayerId{2});
}

TEST(PlayerServer, RefusedRequestReturnsReasonAndNeverCreatesAClient)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	auto *host = static_cast<RakPeer *>(&server.transport());
	auto *client = net.AddPeer(1);
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               {
		EXPECT_TRUE(request.reject("world mismatch"));
		EXPECT_EQ(request.accept(), SessionAcceptResult::Expired); });
	server.on<ClientConnected>([](ClientConnection)
	                           { ADD_FAILURE() << "Refused session became connected"; });
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return net.Received(client, ID_CONNECTION_ATTEMPT_FAILED); }, 5000));
	const auto &data = net.Find(client, ID_CONNECTION_ATTEMPT_FAILED)->data;
	EXPECT_EQ(std::string(data.begin() + 1, data.end()), "world mismatch");
	EXPECT_TRUE(server.players().empty());
}

TEST(PlayerServer, WhitelistAndReservedSlotsShareOnePendingQueue)
{
	SimulatedNetwork net;
	ServerCapacity capacity{2};
	capacity.pending = 4;
	capacity.reserved = 1;
	auto &server = net.AddServer(capacity);
	auto *host = static_cast<RakPeer *>(&server.transport());
	std::vector<ConnectionRequest> requests;
	std::vector<ClientConnection> clients;
	server.on<ConnectionRequested>([&](const ConnectionRequest &request)
	                               { requests.push_back(request); });
	server.on<ClientConnected>([&](ClientConnection client)
	                           { clients.push_back(client); });
	auto arrive = [&](RakPeer *peer)
	{
		const auto before = requests.size();
		return net.StartConnect(peer, host) == CONNECTION_ATTEMPT_STARTED &&
		       net.RunUntil([&]
		                    { server.poll(); return requests.size() > before; }, 5000);
	};
	auto *first = net.AddPeer(1);
	auto *waiting = net.AddPeer(1);
	auto *staff = net.AddPeer(1);
	ASSERT_TRUE(arrive(first));
	ASSERT_TRUE(arrive(waiting));
	ASSERT_TRUE(arrive(staff));
	auto queue = server.pending_requests();
	ASSERT_EQ(queue.size(), 3u);
	for (unsigned i = 0; i < queue.size(); ++i)
		EXPECT_EQ(queue[i].key(), requests[i].key());
	EXPECT_EQ(requests.front().address(), net.AddressOf(first));
	EXPECT_TRUE(clients.empty());
	EXPECT_TRUE(server.players().empty());

	const std::string oversized(MAXIMUM_SESSION_CONFIG_SIZE + 1, 'x');
	EXPECT_EQ(requests[0].accept(ConnectionSlot::Public, oversized), SessionAcceptResult::InvalidPayload);
	EXPECT_EQ(server.pending_requests().size(), 3u);

	// All three decisions happen before the network thread runs again.
	ASSERT_EQ(requests[0].accept(), SessionAcceptResult::Accepted);
	EXPECT_EQ(requests[1].accept(), SessionAcceptResult::Full);
	ASSERT_EQ(requests[2].accept(ConnectionSlot::Reserved), SessionAcceptResult::Accepted);
	queue = server.pending_requests();
	ASSERT_EQ(queue.size(), 1u);
	EXPECT_EQ(queue.front().key(), requests[1].key());
	ASSERT_TRUE(queue.front().status("waiting"));
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return clients.size() == 2 && net.Received(waiting, ID_SESSION_CONFIG_STATUS); }, 5000));
	EXPECT_TRUE(server.players().empty());
	ASSERT_TRUE(arrive(net.AddPeer(1)));
	EXPECT_EQ(requests.back().accept(ConnectionSlot::Reserved), SessionAcceptResult::Full);
	EXPECT_TRUE(requests.back().reject("whitelist"));
	ASSERT_TRUE(clients.front().disconnect());
	ASSERT_TRUE(net.RunUntilReceived(first, ID_DISCONNECTION_NOTIFICATION, 5000));
	ASSERT_EQ(requests[1].accept(), SessionAcceptResult::Accepted);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return clients.size() == 3; }, 5000));
	EXPECT_EQ(clients.back().accept_player(), PlayerId{1});
	EXPECT_TRUE(server.pending_requests().empty());
}
