// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include "TestHelpers.h"
#include <gtest/gtest.h>
#include <thread>
#include <future>

using namespace MafiaNet;

TEST(PlayerServerLive, ListenDeliversJoinAndLeaveOnThePollThread)
{
	auto server = Server::listen(0, {2});
	ASSERT_TRUE(server);
	const auto bound = TestHelpers::BoundPort(&server->transport());
	ASSERT_NE(bound, 0);
	Peer client;
	ASSERT_NE(TestHelpers::StartOnLoopback(client.get(), 1), 0);
	const auto caller = std::this_thread::get_id();
	std::optional<Player> joined;
	std::optional<PlayerLeft> left;
	server->on<ConnectionRequested>([](const ConnectionRequest &request)
	                                { request.accept(); });
	server->on<ClientConnected>([](ClientConnection client)
	                            { client.accept_player(); });
	server->on<PlayerJoined>([&](Player player)
	                         {
		EXPECT_EQ(std::this_thread::get_id(), caller);
		joined = player; });
	server->on<PlayerLeft>([&](const PlayerLeft &event)
	                       {
		EXPECT_EQ(std::this_thread::get_id(), caller);
		left = event; });
	ASSERT_EQ(client->Connect("127.0.0.1", bound, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	bool clientAccepted = false;
	bool clientDisconnected = false;
	const auto wait = [&](auto predicate)
	{
		const auto start = GetTimeMS();
		while (GetTimeMS() - start < 5000)
		{
			server->poll();
			while (auto packet = client.receive())
			{
				if (packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
					clientAccepted = true;
				if (packet->data[0] == ID_DISCONNECTION_NOTIFICATION)
					clientDisconnected = true;
			}
			if (predicate())
				return true;
			RakSleep(5);
		}
		return false;
	};
	ASSERT_TRUE(wait([&]
	                 { return joined.has_value() && clientAccepted; }));
	ASSERT_EQ(joined->id(), PlayerId{1});
	ASSERT_TRUE(joined->disconnect());
	ASSERT_TRUE(wait([&]
	                 { return left.has_value() && clientDisconnected; }));
	EXPECT_EQ(left->player_id, joined->id());
	EXPECT_EQ(left->connection, joined->key());
	EXPECT_FALSE(joined->disconnect());
}

TEST(PlayerServerLive, TwoRawAcceptsCompeteForOneSlotAndTheLoserStaysQueued)
{
	auto server = Server::listen(0, {1, 2});
	ASSERT_TRUE(server);
	auto &transport = server->transport();
	EXPECT_EQ(transport.GetMaximumNumberOfPeers(), 3u);
	std::vector<ConnectionRequest> arrived;
	server->on<ConnectionRequested>([&](const ConnectionRequest &request)
	                                { arrived.push_back(request); });
	const auto port = TestHelpers::BoundPort(&transport);
	Peer clients[2];
	for (auto &client : clients)
	{
		ASSERT_NE(TestHelpers::StartOnLoopback(client.get(), 1), 0);
		ASSERT_EQ(client->Connect("127.0.0.1", port, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	}
	const auto start = GetTimeMS();
	while (arrived.size() < 2 && GetTimeMS() - start < 5000)
	{
		server->poll();
		for (auto &client : clients)
			while (client.receive())
			{
			}
		RakSleep(5);
	}
	ASSERT_EQ(arrived.size(), 2u);
	std::promise<void> ready;
	const auto gate = ready.get_future().share();
	auto attempt = [&](SystemAddress address)
	{
		gate.wait();
		return transport.AcceptSession(address, nullptr, 0);
	};
	auto first = std::async(std::launch::async, attempt, arrived[0].address());
	auto second = std::async(std::launch::async, attempt, arrived[1].address());
	ready.set_value();
	const auto a = first.get();
	const auto b = second.get();
	ASSERT_TRUE((a == SessionAcceptResult::Accepted && b == SessionAcceptResult::Full) ||
	            (a == SessionAcceptResult::Full && b == SessionAcceptResult::Accepted));
	auto pending = server->pending_requests();
	ASSERT_EQ(pending.size(), 1u);
	EXPECT_EQ(pending.front().key(), arrived[a == SessionAcceptResult::Full ? 0 : 1].key());
	EXPECT_TRUE(pending.front().reject("queue closed"));
	EXPECT_TRUE(server->pending_requests().empty());
}
