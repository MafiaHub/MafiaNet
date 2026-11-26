// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/client.h"
#include "mafianet/server.h"
#include "TestHelpers.h"
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace MafiaNet;

namespace
{
struct WorldLoaded
{
	uint32_t revision = 0;
	template <class Archive>
	void serialize(Archive &ar) { ar & revision; }
};
struct Welcome
{
	uint32_t player = 0;
	template <class Archive>
	void serialize(Archive &ar) { ar & player; }
};
constexpr MessageID worldLoadedId = ID_USER_PACKET_ENUM;
constexpr MessageID welcomeId = ID_USER_PACKET_ENUM + 1;

// Poll both ends on this thread until the condition holds or five seconds pass.
template <class Predicate>
bool PollUntil(Server &server, std::vector<Client *> clients, Predicate predicate)
{
	const auto start = GetTimeMS();
	while (GetTimeMS() - start < 5000)
	{
		server.poll();
		for (auto *client : clients)
			client->poll();
		if (predicate())
			return true;
		RakSleep(5);
	}
	return false;
}
}

TEST(PlayerClientLive, SessionRunsFromTicketToTypedLeaveOnThePollThread)
{
	auto server = Server::listen(0, {2});
	ASSERT_TRUE(server);
	const auto port = TestHelpers::BoundPort(&server->transport());
	ASSERT_NE(port, 0);
	const auto caller = std::this_thread::get_id();
	std::optional<Player> joined;
	std::optional<PlayerLeft> left;
	server->on<ConnectionRequested>([](const ConnectionRequest &request)
	                                {
		if (request.payload() != "guest")
			request.reject(3, "Unknown ticket");
		else if (request.status("Checking ticket"))
			request.accept(ConnectionSlot::Public, "world-1"); });
	server->messages().on<WorldLoaded>(worldLoadedId, [&](const WorldLoaded &message, const Sender &sender)
	                                   {
		auto connection = server->connection(sender);
		ASSERT_TRUE(connection);
		EXPECT_EQ(message.revision, 1u);
		connection->accept_player(); });
	server->messages().register_message<Welcome>(welcomeId);
	server->on<PlayerJoined>([&](Player player)
	                         {
		joined = player;
		player.send(Welcome{static_cast<uint32_t>(player.id())}); });
	server->on<PlayerLeft>([&](const PlayerLeft &event)
	                       { left = event; });

	auto peer = Peer::client().host("127.0.0.1").connect("127.0.0.1", port).session("guest").start();
	ASSERT_TRUE(peer);
	Client client(std::move(*peer));
	std::vector<std::string> statuses;
	std::optional<ConnectionAccepted> accepted;
	std::optional<uint32_t> welcomed;
	std::optional<ServerDisconnected> ended;
	client.on<ConnectionStatus>([&](const ConnectionStatus &event)
	                            {
		EXPECT_EQ(std::this_thread::get_id(), caller);
		statuses.push_back(event.update); });
	client.on<ConnectionAccepted>([&](const ConnectionAccepted &event)
	                              {
		accepted = event;
		EXPECT_NE(client.send(WorldLoaded{1}), 0u); });
	client.on<ConnectionFailed>([](const ConnectionFailed &event)
	                            { ADD_FAILURE() << "Connection failed: " << static_cast<int>(event.reason) << ' ' << event.detail; });
	client.on<ServerDisconnected>([&](const ServerDisconnected &event)
	                              { ended = event; });
	client.messages().register_message<WorldLoaded>(worldLoadedId);
	client.messages().on<Welcome>(welcomeId, [&](const Welcome &welcome, const Sender &)
	                              { welcomed = welcome.player; });

	ASSERT_TRUE(PollUntil(*server, {&client}, [&]
	                      { return joined && welcomed; }));
	EXPECT_EQ(statuses, std::vector<std::string>{"Checking ticket"});
	ASSERT_TRUE(accepted);
	EXPECT_EQ(accepted->response, "world-1");
	EXPECT_EQ(*welcomed, static_cast<uint32_t>(joined->id()));

	ASSERT_TRUE(client.disconnect(5, "Quit to menu"));
	ASSERT_TRUE(PollUntil(*server, {&client}, [&]
	                      { return left && ended; }));
	EXPECT_EQ(left->player_id, joined->id());
	EXPECT_EQ(left->reason, LeaveReason::Disconnected);
	EXPECT_EQ(left->code, 5u);
	EXPECT_EQ(left->detail, "Quit to menu");
	EXPECT_EQ(ended->reason, LeaveReason::Closed);
	EXPECT_EQ(ended->code, 5u);

	// The static factory takes the session payload directly; this ticket is refused with a code.
	auto refused = Client::connect("127.0.0.1", port, "stranger");
	ASSERT_TRUE(refused);
	std::optional<ConnectionFailed> failed;
	refused->on<ConnectionFailed>([&](const ConnectionFailed &event)
	                              { failed = event; });
	ASSERT_TRUE(PollUntil(*server, {&*refused}, [&]
	                      { return failed.has_value(); }));
	EXPECT_EQ(failed->reason, ConnectionFailure::Rejected);
	EXPECT_EQ(failed->code, 3u);
	EXPECT_EQ(failed->detail, "Unknown ticket");
	EXPECT_FALSE(refused->connected());
}
