// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/client.h"
#include "mafianet/server.h"
#include "SimulatedNetwork.h"
#include <gtest/gtest.h>
#include <map>

using namespace MafiaNet;

namespace
{
struct Numbered
{
	uint32_t value = 0;
	template <class Archive>
	void serialize(Archive &ar) { ar & value; }
};
constexpr MessageID numberedId = ID_USER_PACKET_ENUM;

// A server that accepts everyone and records the numbers it receives, per sending connection.
struct Rig
{
	SimulatedNetwork net;
	Server &server;
	std::vector<Client *> clients;
	std::map<PeerGuid, std::vector<uint32_t>> received;
	std::vector<uint32_t> arrival;
	std::vector<ConnectionKey> disconnected;

	explicit Rig(unsigned clientCount)
	    : server(net.AddServer({clientCount}))
	{
		server.on<ConnectionRequested>([](const ConnectionRequest &request)
		                               { request.accept(); });
		server.on<ClientConnected>([](ClientConnection connection)
		                           { connection.accept_player(); });
		server.on<ClientDisconnected>([&](const ClientDisconnected &event)
		                              { disconnected.push_back(event.connection); });
		server.messages().on<Numbered>(numberedId, [&](const Numbered &message, const Sender &from)
		                               {
			received[from.peer_guid()].push_back(message.value);
			arrival.push_back(message.value); });
		auto *host = static_cast<RakPeer *>(&server.transport());
		for (unsigned i = 0; i < clientCount; ++i)
		{
			clients.push_back(&net.AddClient(host));
			clients.back()->messages().register_message<Numbered>(numberedId);
		}
		EXPECT_TRUE(net.RunUntil([&]
		                         {
			server.poll();
			bool all = true;
			for (auto *client : clients)
			{
				client->poll();
				all = all && client->connected();
			}
			return all; }, 10000));
	}

	// Client `index` sends `count` numbers base, base + 1, ... without the server polling.
	void send(unsigned index, uint32_t base, unsigned count)
	{
		for (unsigned i = 0; i < count; ++i)
			EXPECT_NE(clients[index]->send(Numbered{base + i}), 0u);
	}

	// Let the network deliver without polling the server, until `expected` packets wait in its transport.
	bool settle(std::size_t expected)
	{
		return net.RunUntil([&]
		                    { return server.backlog() >= expected; }, 10000);
	}
};
}

TEST(ServerPollBudget, DefaultPollHandlesEverythingAndLeavesNoBacklog)
{
	Rig rig(2);
	rig.send(0, 0, 20);
	rig.send(1, 100, 20);
	ASSERT_TRUE(rig.settle(40));
	rig.server.poll();
	EXPECT_EQ(rig.arrival.size(), 40u);
	EXPECT_EQ(rig.server.last_poll().handled, 40u);
	EXPECT_EQ(rig.server.last_poll().backlog, 0u);
	EXPECT_EQ(rig.server.backlog(), 0u);
}

TEST(ServerPollBudget, ZeroBudgetBehavesLikeTheUnbudgetedPoll)
{
	Rig rig(1);
	rig.send(0, 0, 30);
	ASSERT_TRUE(rig.settle(30));
	rig.server.poll(PollBudget{});
	EXPECT_EQ(rig.arrival.size(), 30u);
	EXPECT_EQ(rig.server.backlog(), 0u);
}

TEST(ServerPollBudget, BudgetIsRespectedAndBacklogDrainsInOrderOverSeveralPolls)
{
	Rig rig(1);
	rig.send(0, 0, 30);
	ASSERT_TRUE(rig.settle(30));
	std::size_t previousBacklog = 30;
	unsigned polls = 0;
	while (rig.server.backlog() && polls < 20)
	{
		const std::size_t before = rig.arrival.size();
		rig.server.poll(PollBudget{4, 0});
		++polls;
		EXPECT_LE(rig.arrival.size() - before, 4u);
		EXPECT_EQ(rig.server.last_poll().handled, rig.arrival.size() - before);
		EXPECT_EQ(rig.server.last_poll().backlog, rig.server.backlog());
		EXPECT_LT(rig.server.backlog(), previousBacklog);
		previousBacklog = rig.server.backlog();
	}
	EXPECT_EQ(polls, 8u);
	std::vector<uint32_t> expected(30);
	for (uint32_t i = 0; i < 30; ++i)
		expected[i] = i;
	EXPECT_EQ(rig.arrival, expected);
}

TEST(ServerPollBudget, FloodingConnectionDoesNotStarveAnother)
{
	Rig rig(2);
	rig.send(0, 1000, 40);
	rig.send(1, 2000, 5);
	ASSERT_TRUE(rig.settle(45));
	rig.server.poll(PollBudget{10, 3});
	std::size_t fromFlooder = 0;
	std::size_t fromOther = 0;
	for (uint32_t value : rig.arrival)
		(value >= 2000 ? fromOther : fromFlooder)++;
	EXPECT_EQ(fromFlooder, 3u);
	EXPECT_EQ(fromOther, 3u);
	for (unsigned i = 0; i < 40 && rig.server.backlog(); ++i)
		rig.server.poll(PollBudget{10, 3});
	EXPECT_EQ(rig.server.backlog(), 0u);
	// Nothing was dropped and each connection's own order is intact.
	std::size_t total = 0;
	for (auto &[guid, values] : rig.received)
	{
		total += values.size();
		for (std::size_t i = 1; i < values.size(); ++i)
			EXPECT_EQ(values[i], values[i - 1] + 1);
	}
	EXPECT_EQ(total, 45u);
}

TEST(ServerPollBudget, DisconnectOfAnotherConnectionIsNotHeldBackByAFlood)
{
	Rig rig(2);
	rig.send(0, 0, 40);
	rig.send(1, 1000, 1);
	ASSERT_TRUE(rig.settle(41));
	ASSERT_TRUE(rig.clients[1]->disconnect());
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return rig.server.backlog() >= 42; }, 10000));
	unsigned polls = 0;
	while (rig.disconnected.empty() && polls < 5)
	{
		rig.server.poll(PollBudget{4, 2});
		++polls;
	}
	EXPECT_EQ(rig.disconnected.size(), 1u);
	// The flood is still draining: the event did not wait for it.
	EXPECT_GT(rig.server.backlog(), 0u);
	while (rig.server.backlog())
		rig.server.poll(PollBudget{4, 2});
	EXPECT_EQ(rig.received.size(), 2u);
}

TEST(ServerPollBudget, ADisconnectStaysBehindItsOwnConnectionsParkedPackets)
{
	Rig rig(1);
	rig.send(0, 0, 12);
	ASSERT_TRUE(rig.settle(12));
	ASSERT_TRUE(rig.clients[0]->disconnect());
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return rig.server.backlog() >= 13; }, 10000));
	for (unsigned i = 0; i < 20 && rig.disconnected.empty(); ++i)
	{
		rig.server.poll(PollBudget{3, 0});
		if (rig.disconnected.empty())
			EXPECT_LE(rig.arrival.size(), 12u);
	}
	ASSERT_EQ(rig.disconnected.size(), 1u);
	EXPECT_EQ(rig.arrival.size(), 12u);
}
