// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/inbound_rate_limit.h"
#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/server.h"
#include "mafianet/statistics.h"
#include "SimulatedNetwork.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
using Verdict = InboundRateLimiter::Verdict;
constexpr TimeUS kMs = 1000;

InboundRateLimit Rate(uint32_t perSecond, uint32_t burst = 0)
{
	InboundRateLimit limit;
	limit.messages_per_second = perSecond;
	limit.burst = burst;
	return limit;
}

InboundRateLimit Flood(uint32_t perSecond, FloodAction action, std::string reason = {})
{
	InboundRateLimit limit;
	limit.flood_per_second = perSecond;
	limit.flood_action = action;
	limit.flood_reason = std::move(reason);
	return limit;
}

unsigned CountAccepted(InboundRateLimiter &limiter, const InboundRateLimit &limit, unsigned messages, TimeUS now)
{
	unsigned accepted = 0;
	for (unsigned i = 0; i < messages; ++i)
		accepted += limiter.OnMessage(limit, now) == Verdict::Accept ? 1 : 0;
	return accepted;
}

constexpr MessageID kGameMessage = ID_USER_PACKET_ENUM;

void SendGameMessages(RakPeer *from, const RakPeer *to, SimulatedNetwork &net, unsigned count, bool timestamped = false)
{
	for (unsigned i = 0; i < count; ++i)
	{
		BitStream out;
		if (timestamped)
		{
			out.Write(MessageID(ID_TIMESTAMP));
			out.Write(GetTime());
		}
		out.Write(kGameMessage);
		out.Write(i);
		ASSERT_NE(from->Send(&out, Priority::High, Reliability::ReliableOrdered, 0, net.AddressOf(to), false), 0u);
	}
}

size_t CountReceived(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	size_t count = 0;
	for (const auto &message : net.Inbox(peer))
		count += message.id == id ? 1 : 0;
	return count;
}

size_t CountGameMessages(SimulatedNetwork &net, const RakPeer *peer)
{
	size_t count = 0;
	for (const auto &message : net.Inbox(peer))
	{
		const bool stamped = message.id == ID_TIMESTAMP && message.data.size() > 1 + sizeof(Time) && message.data[1 + sizeof(Time)] == kGameMessage;
		count += message.id == kGameMessage || stamped ? 1 : 0;
	}
	return count;
}

RakNetStatistics StatisticsFor(RakPeer *peer, const RakPeer *remote, SimulatedNetwork &net)
{
	RakNetStatistics statistics{};
	EXPECT_NE(peer->GetStatistics(net.AddressOf(remote), &statistics), nullptr);
	return statistics;
}
}

TEST(InboundRateLimiter, DisabledLimitAcceptsEverything)
{
	InboundRateLimiter limiter;
	EXPECT_FALSE(InboundRateLimit{}.enabled());
	EXPECT_EQ(CountAccepted(limiter, InboundRateLimit{}, 100000, 0), 100000u);
	EXPECT_EQ(limiter.Dropped(), 0u);
}

TEST(InboundRateLimiter, StartsWithAFullBurstThenRefillsAtTheSustainedRate)
{
	InboundRateLimiter limiter;
	const auto limit = Rate(10, 5);
	EXPECT_EQ(CountAccepted(limiter, limit, 8, 0), 5u);
	// One message every 100 ms.
	EXPECT_EQ(CountAccepted(limiter, limit, 3, 99 * kMs), 0u);
	EXPECT_EQ(CountAccepted(limiter, limit, 3, 100 * kMs), 1u);
	EXPECT_EQ(CountAccepted(limiter, limit, 3, 350 * kMs), 2u);
	// An idle connection banks at most a burst.
	EXPECT_EQ(CountAccepted(limiter, limit, 20, 60000 * kMs), 5u);
	EXPECT_EQ(limiter.Dropped(), 3u + 3u + 2u + 1u + 15u);
	EXPECT_EQ(limiter.Floods(), 0u);
}

TEST(InboundRateLimiter, BurstDefaultsToTheSustainedRate)
{
	InboundRateLimiter limiter;
	EXPECT_EQ(CountAccepted(limiter, Rate(7), 20, 0), 7u);
}

TEST(InboundRateLimiter, FloodIsReportedOncePerWindowAndDropsTheRestOfIt)
{
	InboundRateLimiter limiter;
	const auto limit = Flood(20, FloodAction::Drop);
	EXPECT_EQ(CountAccepted(limiter, limit, 20, 0), 20u);
	EXPECT_EQ(limiter.OnMessage(limit, 1 * kMs), Verdict::Flood);
	EXPECT_EQ(limiter.OnMessage(limit, 2 * kMs), Verdict::Drop);
	EXPECT_EQ(limiter.OnMessage(limit, 999 * kMs), Verdict::Drop);
	// The next window starts clean.
	EXPECT_EQ(CountAccepted(limiter, limit, 20, 1000 * kMs), 20u);
	EXPECT_EQ(limiter.OnMessage(limit, 1001 * kMs), Verdict::Flood);
	EXPECT_EQ(limiter.Floods(), 2u);
	EXPECT_EQ(limiter.Dropped(), 4u);
}

TEST(InboundRateLimiter, MessagesDroppedByTheRateStillCountTowardsAFlood)
{
	InboundRateLimiter limiter;
	auto limit = Rate(1, 1);
	limit.flood_per_second = 5;
	EXPECT_EQ(limiter.OnMessage(limit, 0), Verdict::Accept);
	for (int i = 0; i < 4; ++i)
		EXPECT_EQ(limiter.OnMessage(limit, 0), Verdict::Drop);
	EXPECT_EQ(limiter.OnMessage(limit, 0), Verdict::Flood);
}

TEST(InboundRateLimiter, ExtremeRatesAndClockJumpsDoNotOverflow)
{
	InboundRateLimiter limiter;
	const auto limit = Rate(UINT32_MAX, UINT32_MAX);
	EXPECT_EQ(limiter.OnMessage(limit, 0), Verdict::Accept);
	EXPECT_EQ(limiter.OnMessage(limit, UINT64_MAX / 2), Verdict::Accept);
	// A clock that steps back neither refills nor underflows.
	EXPECT_EQ(limiter.OnMessage(limit, 5), Verdict::Accept);
	InboundRateLimiter slow;
	EXPECT_EQ(slow.OnMessage(Rate(1), 0), Verdict::Accept);
	EXPECT_EQ(slow.OnMessage(Rate(1), UINT64_MAX), Verdict::Accept);
	EXPECT_EQ(slow.OnMessage(Rate(1), UINT64_MAX), Verdict::Drop);
}

TEST(InboundRateLimiter, ShrinkingTheBurstCapsWhatIsBanked)
{
	InboundRateLimiter limiter;
	EXPECT_EQ(CountAccepted(limiter, Rate(100, 100), 1, 0), 1u);
	EXPECT_EQ(CountAccepted(limiter, Rate(100, 3), 10, 0), 3u);
}

TEST(InboundRateLimit, BurstWithoutARateIsRejected)
{
	SimulatedNetwork net;
	auto *peer = net.AddPeer(1, 1);
	EXPECT_FALSE(peer->SetInboundRateLimit(Rate(0, 10)));
	EXPECT_FALSE(peer->GetInboundRateLimit().enabled());
	auto limit = Flood(10, FloodAction::Disconnect, "flood");
	ASSERT_TRUE(peer->SetInboundRateLimit(limit));
	EXPECT_EQ(peer->GetInboundRateLimit().flood_reason, "flood");
	EXPECT_EQ(peer->GetInboundRateLimit().flood_action, FloodAction::Disconnect);
}

TEST(InboundRateLimit, ExcessMessagesAreDroppedBeforeReceiveAndCounted)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(server->SetInboundRateLimit(Rate(10)));
	ASSERT_TRUE(net.Connect(client, server));

	SendGameMessages(client, server, net, 30);
	SendGameMessages(client, server, net, 20, true);
	net.Step(50);
	// The full burst, and nothing refilled in 50 ms at 10 per second.
	EXPECT_EQ(CountGameMessages(net, server), 10u);
	auto statistics = StatisticsFor(server, client, net);
	EXPECT_EQ(statistics.rateLimitedMessages, 40u);
	EXPECT_EQ(statistics.inboundFloods, 0u);

	// Internal traffic is never limited: the connection keeps answering pings with an exhausted budget.
	server->Ping(net.AddressOf(client));
	net.Step(100);
	EXPECT_NE(server->GetLastPing(net.AddressOf(client)), -1);
	EXPECT_EQ(server->GetConnectionState(net.AddressOf(client)), IS_CONNECTED);

	// Paced at the sustained rate, every message arrives.
	net.Inbox(server).clear();
	for (int i = 0; i < 10; ++i)
	{
		net.Step(100);
		SendGameMessages(client, server, net, 1);
	}
	net.Step(50);
	EXPECT_EQ(CountGameMessages(net, server), 10u);
	EXPECT_EQ(StatisticsFor(server, client, net).rateLimitedMessages, 40u);
}

TEST(InboundRateLimit, EachConnectionHasItsOwnBudget)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(2, 2);
	auto *noisy = net.AddPeer(1);
	auto *quiet = net.AddPeer(1);
	ASSERT_TRUE(server->SetInboundRateLimit(Rate(5)));
	ASSERT_TRUE(net.Connect(noisy, server));
	ASSERT_TRUE(net.Connect(quiet, server));
	SendGameMessages(noisy, server, net, 50);
	SendGameMessages(quiet, server, net, 5);
	net.Step(50);
	size_t fromNoisy = 0, fromQuiet = 0;
	for (const auto &message : net.Inbox(server))
	{
		if (message.id != kGameMessage)
			continue;
		fromNoisy += message.guid == noisy->GetMyGUID() ? 1 : 0;
		fromQuiet += message.guid == quiet->GetMyGUID() ? 1 : 0;
	}
	EXPECT_EQ(fromNoisy, 5u);
	EXPECT_EQ(fromQuiet, 5u);
	EXPECT_EQ(StatisticsFor(server, noisy, net).rateLimitedMessages, 45u);
	EXPECT_EQ(StatisticsFor(server, quiet, net).rateLimitedMessages, 0u);
	RakNetStatistics total{};
	ASSERT_NE(server->GetStatistics(UNASSIGNED_SYSTEM_ADDRESS, &total), nullptr);
	EXPECT_EQ(total.rateLimitedMessages, 45u);
}

TEST(InboundRateLimit, FloodWithDropActionMutesTheRestOfTheWindow)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(server->SetInboundRateLimit(Flood(20, FloodAction::Drop)));
	ASSERT_TRUE(net.Connect(client, server));

	SendGameMessages(client, server, net, 30);
	net.Step(50);
	EXPECT_EQ(CountGameMessages(net, server), 20u);
	auto statistics = StatisticsFor(server, client, net);
	EXPECT_EQ(statistics.inboundFloods, 1u);
	EXPECT_EQ(statistics.rateLimitedMessages, 10u);

	net.Inbox(server).clear();
	net.Step(1000);
	SendGameMessages(client, server, net, 5);
	net.Step(50);
	EXPECT_EQ(CountGameMessages(net, server), 5u);
	EXPECT_EQ(server->GetConnectionState(net.AddressOf(client)), IS_CONNECTED);
	EXPECT_EQ(CountReceived(net, client, ID_DISCONNECTION_NOTIFICATION), 0u);
}

TEST(InboundRateLimit, FloodWithDisconnectActionClosesWithTheReasonOnBothEnds)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(server->SetInboundRateLimit(Flood(20, FloodAction::Disconnect, "flood")));
	ASSERT_TRUE(net.Connect(client, server));

	SendGameMessages(client, server, net, 100);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 5000));
	ASSERT_TRUE(net.RunUntilReceived(client, ID_DISCONNECTION_NOTIFICATION, 5000));
	EXPECT_EQ(CountGameMessages(net, server), 20u);

	const auto *local = net.Find(server, ID_DISCONNECTION_NOTIFICATION);
	EXPECT_EQ(local->disconnectCause, DisconnectCause::InboundFlood);
	EXPECT_EQ(std::string(local->data.begin() + 1, local->data.end()), "flood");
	const auto *remote = net.Find(client, ID_DISCONNECTION_NOTIFICATION);
	EXPECT_EQ(remote->disconnectCause, DisconnectCause::Unspecified);
	EXPECT_EQ(std::string(remote->data.begin() + 1, remote->data.end()), "flood");

	// Reported once, and nothing the client sent after the flood reached the application.
	net.Step(2000);
	EXPECT_EQ(CountReceived(net, server, ID_DISCONNECTION_NOTIFICATION), 1u);
	EXPECT_EQ(CountGameMessages(net, server), 20u);
	EXPECT_EQ(server->GetConnectionState(net.AddressOf(client)), IS_DISCONNECTED);
}

TEST(InboundRateLimit, AnApplicationCloseIsNotReportedAsAFlood)
{
	SimulatedNetwork net;
	auto *server = net.AddPeer(1, 1);
	auto *client = net.AddPeer(1);
	ASSERT_TRUE(server->SetInboundRateLimit(Flood(20, FloodAction::Disconnect, "flood")));
	ASSERT_TRUE(net.Connect(client, server));
	SendGameMessages(client, server, net, 5);
	net.Step(50);
	server->CloseConnection(net.AddressOf(client), true);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 5000));
	const auto *local = net.Find(server, ID_DISCONNECTION_NOTIFICATION);
	EXPECT_EQ(local->disconnectCause, DisconnectCause::Unspecified);
	EXPECT_EQ(local->data.size(), 1u);
}

TEST(InboundRateLimit, ServerReportsAFloodedPlayerLeaving)
{
	SimulatedNetwork net;
	auto &server = net.AddServer({1});
	auto *host = static_cast<RakPeer *>(&server.transport());
	ASSERT_TRUE(host->SetInboundRateLimit(Flood(10, FloodAction::Disconnect, "flood")));
	auto *client = net.AddPeer(1);
	std::vector<PlayerLeft> left;
	std::vector<ClientDisconnected> disconnected;
	server.on<ConnectionRequested>([](const ConnectionRequest &request)
	                               { EXPECT_EQ(request.accept(), SessionAcceptResult::Accepted); });
	server.on<ClientConnected>([](ClientConnection connection)
	                           { EXPECT_TRUE(connection.accept_player()); });
	server.on<PlayerLeft>([&](const PlayerLeft &event)
	                      { left.push_back(event); });
	server.on<ClientDisconnected>([&](const ClientDisconnected &event)
	                              { disconnected.push_back(event); });
	ASSERT_EQ(net.StartConnect(client, host), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED) && server.players().size() == 1; }, 5000));

	SendGameMessages(client, host, net, 50);
	ASSERT_TRUE(net.RunUntil([&]
	                         { server.poll(); return !disconnected.empty(); }, 5000));
	ASSERT_EQ(left.size(), 1u);
	EXPECT_EQ(left[0].player_id, PlayerId{1});
	EXPECT_EQ(left[0].reason, LeaveReason::InboundFlood);
	EXPECT_EQ(left[0].detail, "flood");
	EXPECT_EQ(disconnected[0].reason, LeaveReason::InboundFlood);
	EXPECT_TRUE(server.players().empty());
}

TEST(InboundRateLimit, ServerBuilderRejectsAnInvalidLimit)
{
	auto started = Peer::server().port(0).inbound_rate_limit(Rate(0, 5)).start();
	ASSERT_FALSE(started);
	EXPECT_EQ(started.error().startup, STARTUP_OTHER_FAILURE);
}
