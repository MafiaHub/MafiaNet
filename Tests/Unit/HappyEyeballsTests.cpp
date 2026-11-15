/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Connect() on a dual-stack socket races the resolved IPv6 and IPv4
 *  candidates (RFC 8305, "Happy Eyeballs"). Every scenario runs on the
 *  SimulatedNetwork: dual-stack peers bind 10.0.0.<n> and 2001:db8::<n>, and
 *  host names resolve through the network's own table, so nothing touches DNS.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
size_t Count(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	size_t n = 0;
	for (const SimulatedNetwork::Message &m : net.Inbox(peer))
		if (m.id == id)
			++n;
	return n;
}

SimulatedNetwork::LinkRule Dark()
{
	SimulatedNetwork::LinkRule rule;
	rule.dark = true;
	return rule;
}

SimulatedNetwork::LinkRule Delay(uint32_t ms)
{
	SimulatedNetwork::LinkRule rule;
	rule.delayMs = ms;
	return rule;
}

// Every ID_OPEN_CONNECTION_REQUEST_1 any peer sends: where to, and when. Also
// every ID_OPEN_CONNECTION_REQUEST_2, which only follows a reply.
struct RequestLog
{
	struct Entry
	{
		SystemAddress from;
		SystemAddress to;
		TimeUS at;
	};
	std::vector<Entry> secondRequests;
	std::vector<Entry> entries;

	void Attach(SimulatedNetwork &net)
	{
		net.SetTap([this, &net](const SystemAddress &from, const SystemAddress &to, const char *data, int length)
		           {
			           if (length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REQUEST_1)
				           entries.push_back({from, to, net.Now()});
			           if (length > 0 && (unsigned char) data[0] == ID_OPEN_CONNECTION_REQUEST_2)
				           secondRequests.push_back({from, to, net.Now()}); });
	}

	size_t CountTo(const SystemAddress &to) const
	{
		size_t n = 0;
		for (const Entry &e : entries)
			if (e.to == to)
				++n;
		return n;
	}

	const Entry *FirstTo(const SystemAddress &to) const
	{
		for (const Entry &e : entries)
			if (e.to == to)
				return &e;
		return nullptr;
	}
};
} // namespace

TEST(HappyEyeballs, PrefersIpv6WhenBothWork)
{
	SimulatedNetwork net;
	// A slow path, so the IPv4 candidate starts (at 250 ms) before the IPv6
	// reply arrives (at ~400 ms): the race is really run, and IPv6 still wins.
	net.SetDefaultLink(Delay(200));
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	ASSERT_EQ(server6.GetIPVersion(), 6);
	ASSERT_EQ(server4.GetIPVersion(), 4);
	net.AddHostName("srv.test", {server6, server4});

	RequestLog log;
	log.Attach(net);

	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	net.Step(5000);

	const SimulatedNetwork::Message *accepted = net.Find(client, ID_CONNECTION_REQUEST_ACCEPTED);
	ASSERT_NE(accepted, nullptr);
	EXPECT_EQ(accepted->systemAddress.GetIPVersion(), 6);
	EXPECT_TRUE(accepted->systemAddress == server6);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED), 1u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 0u);
	EXPECT_EQ(Count(net, server, ID_NEW_INCOMING_CONNECTION), 1u);
	EXPECT_EQ(server->NumberOfConnections(), 1u);
	// Both candidates were tried; the losing one was cancelled after one
	// request, so its late reply led nowhere.
	EXPECT_GE(log.CountTo(server6), 1u);
	EXPECT_EQ(log.CountTo(server4), 1u);
	ASSERT_FALSE(log.secondRequests.empty());
	for (const RequestLog::Entry &e : log.secondRequests)
		EXPECT_TRUE(e.to == server6) << e.to.ToString(true);
	EXPECT_EQ(net.Inbox(client).size(), Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED)) << "unexpected extra message";
}

TEST(HappyEyeballs, FallsBackToIpv4WhenIpv6Dark)
{
	const uint32_t delayMs = 10;
	SimulatedNetwork net;
	net.SetDefaultLink(Delay(delayMs));
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	net.AddHostName("srv.test", {server6, server4});

	const TimeUS start = net.Now();
	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED); }, 5000, 1));
	const TimeUS elapsedMs = (net.Now() - start) / 1000;
	EXPECT_LT(elapsedMs, (TimeUS) (RakPeer::HAPPY_EYEBALLS_ATTEMPT_DELAY_MS + 3 * delayMs + 100));

	// Long enough for the IPv6 candidate's whole retry budget to have run out.
	net.Step(15000);
	const SimulatedNetwork::Message *accepted = net.Find(client, ID_CONNECTION_REQUEST_ACCEPTED);
	ASSERT_NE(accepted, nullptr);
	EXPECT_TRUE(accepted->systemAddress == server4);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED), 1u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 0u);
	EXPECT_EQ(Count(net, server, ID_NEW_INCOMING_CONNECTION), 1u);
}

// The application only knows the race by the first candidate's address (Connect() reports failures under it): once a
// later candidate wins, GetConnectionState() of that address follows the connection, until it closes.
TEST(HappyEyeballs, ConnectionStateOfFirstAddressFollowsTheWinner)
{
	SimulatedNetwork net;
	net.SetDefaultLink(Delay(10));
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	net.AddHostName("srv.test", {server6, server4});

	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	EXPECT_EQ(client->GetConnectionState(server6), IS_PENDING);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	ASSERT_TRUE(net.Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->systemAddress == server4);

	EXPECT_EQ(client->GetConnectionState(server4), IS_CONNECTED);
	EXPECT_EQ(client->GetConnectionState(server6), IS_CONNECTED);

	client->CloseConnection(server4, true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return client->GetConnectionState(server4) != IS_CONNECTED && client->GetConnectionState(server4) != IS_DISCONNECTING && client->NumberOfConnections() == 0; }, 5000));
	EXPECT_EQ(client->GetConnectionState(server6), IS_NOT_CONNECTED);
}

// A closed connection leaves its address in its (inactive) slot. When the race that follows is won by a later
// candidate, GetConnectionState() of the first address must report the live connection, not that closed slot.
TEST(HappyEyeballs, ConnectionStateOfFirstAddressIgnoresAnEarlierClosedConnection)
{
	SimulatedNetwork net;
	net.SetDefaultLink(Delay(10));
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *other = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(2);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	const SystemAddress other4 = net.AddressOf(other, 4);
	net.AddHostName("srv.test", {server6, server4});

	// Slot 0: other, slot 1: server6 by literal (not raced). Close both, server6 first, so the race below reuses
	// slot 0 and slot 1 keeps server6 as a closed slot.
	ASSERT_EQ(client->Connect(other4.ToString(false), other4.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return client->GetConnectionState(other4) == IS_CONNECTED; }, 5000));
	ASSERT_EQ(client->Connect(server6.ToString(false), server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return client->GetConnectionState(server6) == IS_CONNECTED; }, 5000));
	client->CloseConnection(server6, true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return client->GetConnectionState(server6) == IS_DISCONNECTED; }, 5000));
	client->CloseConnection(other4, true);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return client->GetConnectionState(other4) == IS_DISCONNECTED; }, 5000));
	net.Step(1000);

	// Now IPv6 is dark: the race's IPv4 candidate wins.
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntil([&]
	                         { return client->GetConnectionState(server4) == IS_CONNECTED; }, 5000));
	EXPECT_EQ(client->GetConnectionState(server6), IS_CONNECTED);
}

TEST(HappyEyeballs, NextCandidateStartsWhenPreviousFails)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	net.AddHostName("srv.test", {server6, server4});

	RequestLog log;
	log.Attach(net);

	// Two requests 40 ms apart: the IPv6 candidate gives up well before the
	// 250 ms attempt delay, and the IPv4 one must not wait for it.
	const TimeUS start = net.Now();
	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0, nullptr, 0, 1, 40), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));

	const RequestLog::Entry *first4 = log.FirstTo(server4);
	ASSERT_NE(first4, nullptr);
	EXPECT_LT((first4->at - start) / 1000, (TimeUS) RakPeer::HAPPY_EYEBALLS_ATTEMPT_DELAY_MS);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 0u);
}

TEST(HappyEyeballs, SingleFailureAfterAllCandidatesFail)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	net.SetLinkBothWays(net.AddressOf(client, 4), server4, Dark());
	net.AddHostName("srv.test", {server6, server4});

	RequestLog log;
	log.Attach(net);

	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	EXPECT_EQ(client->GetConnectionState(server6), IS_PENDING);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_ATTEMPT_FAILED, 30000));
	net.Step(10000);

	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 1u);
	const SimulatedNetwork::Message *failed = net.Find(client, ID_CONNECTION_ATTEMPT_FAILED);
	ASSERT_NE(failed, nullptr);
	EXPECT_TRUE(failed->systemAddress == server6) << failed->systemAddress.ToString(true);
	// Both candidates spent their whole budget.
	EXPECT_EQ(log.CountTo(server6), 7u);
	EXPECT_EQ(log.CountTo(server4), 7u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED), 0u);
	EXPECT_NE(client->GetConnectionState(server6), IS_PENDING);
	EXPECT_NE(client->GetConnectionState(server4), IS_PENDING);
}

TEST(HappyEyeballs, CancelStopsWholeRace)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	// IPv6 is dark, so the race is still on at 100 ms; IPv4 works and would
	// connect at 250 ms if the cancel left its candidate behind.
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	net.AddHostName("srv.test", {server6, server4});

	RequestLog log;
	log.Attach(net);

	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	net.Step(100);
	ASSERT_EQ(client->GetConnectionState(server6), IS_PENDING);
	client->CancelConnectionAttempt(server6);
	const size_t requestsAtCancel = log.entries.size();
	EXPECT_NE(client->GetConnectionState(server6), IS_PENDING);
	EXPECT_NE(client->GetConnectionState(server4), IS_PENDING);

	net.Step(10000);
	EXPECT_EQ(log.entries.size(), requestsAtCancel);
	EXPECT_EQ(log.CountTo(server4), 0u);
	EXPECT_EQ(Count(net, server, ID_NEW_INCOMING_CONNECTION), 0u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED), 0u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 0u);
}

TEST(HappyEyeballs, ReconnectRightAfterCancelStartsAFreshRace)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());
	net.AddHostName("srv.test", {server6, server4});

	RequestLog log;
	log.Attach(net);

	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	net.Step(100);
	client->CancelConnectionAttempt(server6);
	// The cancelled race is gone as far as the caller can tell, before the
	// network thread has even dropped it.
	EXPECT_NE(client->GetConnectionState(server6), IS_PENDING);
	const TimeUS reconnectAt = net.Now();
	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	EXPECT_EQ(client->GetConnectionState(server6), IS_PENDING);

	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	net.Step(15000);

	// The new race's IPv4 candidate started on its own schedule, and only once.
	const RequestLog::Entry *first4 = log.FirstTo(server4);
	ASSERT_NE(first4, nullptr);
	EXPECT_GE((first4->at - reconnectAt) / 1000, (TimeUS) RakPeer::HAPPY_EYEBALLS_ATTEMPT_DELAY_MS);
	EXPECT_EQ(log.CountTo(server4), 1u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED), 1u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 0u);
	EXPECT_EQ(Count(net, server, ID_NEW_INCOMING_CONNECTION), 1u);
	EXPECT_EQ(server->NumberOfConnections(), 1u);
	EXPECT_EQ(client->NumberOfConnections(), 1u);
}

TEST(HappyEyeballs, ServerRejectionEndsTheRace)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	const SystemAddress server4 = net.AddressOf(server, 4);
	const SystemAddress client6 = net.AddressOf(client, 6);
	// IPv6 answers at ~400 ms, after the IPv4 candidate started (250 ms) but
	// long before IPv4 could answer (~2250 ms).
	net.SetLinkBothWays(client6, server6, Delay(200));
	net.SetLinkBothWays(net.AddressOf(client, 4), server4, Delay(1000));
	net.AddHostName("srv.test", {server6, server4});

	char client6Ip[64];
	client6.ToString(false, client6Ip, sizeof(client6Ip));
	server->AddToBanList(client6Ip);

	RequestLog log;
	log.Attach(net);

	ASSERT_EQ(client->Connect("srv.test", server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_BANNED, 5000));
	const TimeUS bannedAt = net.Now();
	ASSERT_GE(log.CountTo(server4), 1u) << "the IPv4 candidate should have started before the rejection";
	net.Step(15000);

	EXPECT_EQ(Count(net, client, ID_CONNECTION_BANNED), 1u);
	EXPECT_TRUE(net.Find(client, ID_CONNECTION_BANNED)->systemAddress == server6);
	for (const RequestLog::Entry &e : log.entries)
		EXPECT_LT(e.at, bannedAt) << "request to " << e.to.ToString(true) << " after the rejection";
	EXPECT_TRUE(log.secondRequests.empty());
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 0u);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_REQUEST_ACCEPTED), 0u);
	EXPECT_EQ(Count(net, server, ID_NEW_INCOMING_CONNECTION), 0u);
	EXPECT_NE(client->GetConnectionState(server6), IS_PENDING);
}

TEST(HappyEyeballs, LiteralAddressNotRaced)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server6 = net.AddressOf(server, 6);
	// Dark, so a race (were there one) would reach for another address.
	net.SetLinkBothWays(net.AddressOf(client, 6), server6, Dark());

	RequestLog log;
	log.Attach(net);

	char literal[64];
	server6.ToString(false, literal, sizeof(literal));
	ASSERT_EQ(client->Connect(literal, server6.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_ATTEMPT_FAILED, 30000));

	ASSERT_FALSE(log.entries.empty());
	for (const RequestLog::Entry &e : log.entries)
		EXPECT_TRUE(e.to == server6) << e.to.ToString(true);
	EXPECT_EQ(Count(net, client, ID_CONNECTION_ATTEMPT_FAILED), 1u);
}

TEST(HappyEyeballs, SingleCandidateHostNameIsNotRaced)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeerDualStack(4, 4);
	RakPeer *client = net.AddPeerDualStack(1);
	const SystemAddress server4 = net.AddressOf(server, 4);
	net.AddHostName("v4only.test", {server4});

	ASSERT_EQ(client->Connect("v4only.test", server4.GetPort(), nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_REQUEST_ACCEPTED, 5000));
	EXPECT_TRUE(net.Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->systemAddress == server4);
}

TEST(HappyEyeballs, UnknownHostNameCannotResolve)
{
	SimulatedNetwork net;
	RakPeer *client = net.AddPeerDualStack(1);
	EXPECT_EQ(client->Connect("nowhere.test", 60000, nullptr, 0), CANNOT_RESOLVE_DOMAIN_NAME);
}

// The dual-stack resolution path (one socket, either family, chosen by the
// literal) against single-family servers of each family.
TEST(HappyEyeballs, DualStackClientConnectsByLiteralToEitherFamily)
{
	SimulatedNetwork net;
	RakPeer *server4 = net.AddPeer(4, 4);
	RakPeer *server6 = net.AddPeerV6(4, 4);
	RakPeer *client = net.AddPeerDualStack(2);

	ASSERT_TRUE(net.Connect(client, server4));
	ASSERT_TRUE(net.Connect(client, server6));

	EXPECT_TRUE(net.Find(server4, ID_NEW_INCOMING_CONNECTION)->systemAddress == net.AddressOf(client, 4));
	EXPECT_TRUE(net.Find(server6, ID_NEW_INCOMING_CONNECTION)->systemAddress == net.AddressOf(client, 6));
	EXPECT_EQ(client->NumberOfConnections(), 2u);
}
