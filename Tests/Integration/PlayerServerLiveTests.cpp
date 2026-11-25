// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include "TestHelpers.h"
#include <gtest/gtest.h>
#include <thread>

using namespace MafiaNet;

TEST(PlayerServerLive, ListenDeliversJoinAndLeaveOnThePollThread)
{
	auto server = Server::listen(0, 2);
	ASSERT_TRUE(server);
	const auto bound = TestHelpers::BoundPort(&server->transport());
	ASSERT_NE(bound, 0);
	Peer client;
	ASSERT_NE(TestHelpers::StartOnLoopback(client.get(), 1), 0);
	const auto caller = std::this_thread::get_id();
	std::optional<Player> joined;
	std::optional<PlayerLeft> left;
	server->on<PlayerJoined>([&](Player player)
	                         {
		EXPECT_EQ(std::this_thread::get_id(), caller);
		joined = player; });
	server->on<PlayerLeft>([&](const PlayerLeft &event)
	                       {
		EXPECT_EQ(std::this_thread::get_id(), caller);
		left = event; });
	ASSERT_EQ(client->Connect("127.0.0.1", bound, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	const auto wait = [&](auto predicate)
	{
		const auto start = GetTimeMS();
		while (GetTimeMS() - start < 5000)
		{
			server->poll();
			while (client.receive())
			{
			}
			if (predicate())
				return true;
			RakSleep(5);
		}
		return false;
	};
	ASSERT_TRUE(wait([&]
	                 { return joined.has_value(); }));
	ASSERT_EQ(joined->id(), PlayerId{1});
	ASSERT_TRUE(joined->disconnect());
	ASSERT_TRUE(wait([&]
	                 { return left.has_value(); }));
	EXPECT_EQ(left->player_id, joined->id());
	EXPECT_EQ(left->connection, joined->key());
	EXPECT_FALSE(joined->disconnect());
}
