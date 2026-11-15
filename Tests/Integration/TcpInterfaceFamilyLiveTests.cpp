/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  TCPInterface address families over real loopback connections: a host name
 *  connect with the default AF_INET (the first answer for "localhost" may be
 *  ::1), and an AF_UNSPEC listener that accepts IPv4 and IPv6 clients.
 */

#include <gtest/gtest.h>

#include "TcpTestHelpers.h"
#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/socket_includes.h"
#include "mafianet/tcp_interface.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h> // getaddrinfo
#else
#include <netdb.h> // getaddrinfo
#endif

using namespace MafiaNet;

using TcpTestHelpers::HasIpv6Loopback;
using TcpTestHelpers::ListeningTcp;

class TcpInterfaceFamilyLive : public ::testing::Test
{
protected:
	void SetUp() override
	{
#ifdef _WIN32
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		server.reset(new ListeningTcp);
		client.reset(new TCPInterface);
		client6.reset(new TCPInterface);
	}

	void TearDown() override
	{
		for (TCPInterface *tcp : {client6.get(), client.get(), static_cast<TCPInterface *>(server.get())})
			tcp->Stop();
		client6.reset();
		client.reset();
		server.reset();
#ifdef _WIN32
		WSACleanup();
#endif
	}

	// Polls the server until it reports \a count new incoming connections (or 5 s pass).
	std::vector<SystemAddress> WaitForIncoming(size_t count)
	{
		std::vector<SystemAddress> incoming;
		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 5000 && incoming.size() < count)
		{
			const SystemAddress a = server->HasNewIncomingConnection();
			if (a != UNASSIGNED_SYSTEM_ADDRESS)
				incoming.push_back(a);
			else
				RakSleep(30);
		}
		return incoming;
	}

	std::unique_ptr<ListeningTcp> server;
	std::unique_ptr<TCPInterface> client, client6;
};

// The default family is AF_INET: a host name must resolve to its IPv4 address,
// even where the resolver lists ::1 first for "localhost".
TEST_F(TcpInterfaceFamilyLive, BlockingConnectByHostNameUsesIpv4)
{
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0));

	const SystemAddress serverSide = client->Connect("localhost", port, true, AF_INET);
	ASSERT_NE(serverSide, UNASSIGNED_SYSTEM_ADDRESS) << "Connect(\"localhost\", AF_INET) failed";
	EXPECT_EQ(serverSide.GetIPVersion(), 4);
	EXPECT_TRUE(serverSide.IsLoopback());
	EXPECT_EQ(serverSide.GetPort(), port);
	EXPECT_EQ(WaitForIncoming(1).size(), 1u);
}

TEST_F(TcpInterfaceFamilyLive, NonBlockingConnectByHostNameUsesIpv4)
{
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0));

	client->Connect("localhost", port, false, AF_INET);
	SystemAddress completed = UNASSIGNED_SYSTEM_ADDRESS, failed = UNASSIGNED_SYSTEM_ADDRESS;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && completed == UNASSIGNED_SYSTEM_ADDRESS && failed == UNASSIGNED_SYSTEM_ADDRESS)
	{
		completed = client->HasCompletedConnectionAttempt();
		failed = client->HasFailedConnectionAttempt();
		if (completed == UNASSIGNED_SYSTEM_ADDRESS && failed == UNASSIGNED_SYSTEM_ADDRESS)
			RakSleep(30);
	}
	ASSERT_EQ(failed, UNASSIGNED_SYSTEM_ADDRESS) << "Connect(\"localhost\", AF_INET) failed";
	ASSERT_NE(completed, UNASSIGNED_SYSTEM_ADDRESS) << "the connection attempt never completed";
	EXPECT_EQ(completed.GetIPVersion(), 4);
	EXPECT_EQ(completed.GetPort(), port);
}

// Whether the resolver lists ::1 among the TCP answers for "localhost" (it usually lists it first).
static bool LocalhostResolvesToIpv6Loopback()
{
	addrinfo hints, *res = nullptr;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo("localhost", "1", &hints, &res) != 0)
		return false;
	bool found = false;
	static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	for (addrinfo *ai = res; ai != nullptr && !found; ai = ai->ai_next)
		found = ai->ai_family == AF_INET6 && memcmp(((const sockaddr_in6 *) ai->ai_addr)->sin6_addr.s6_addr, loopback, sizeof(loopback)) == 0;
	freeaddrinfo(res);
	return found;
}

// A host name connect tries every address the name resolves to, not only the first: with AF_UNSPEC "localhost"
// resolves to ::1 (nobody listens there) and 127.0.0.1 (the listener), and must reach the listener whatever the
// resolver's order.
TEST_F(TcpInterfaceFamilyLive, BlockingUnspecConnectByHostNameTriesEveryAddress)
{
	if (!LocalhostResolvesToIpv6Loopback())
		GTEST_SKIP() << "\"localhost\" has no ::1 answer on this host";
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0));

	const SystemAddress serverSide = client->Connect("localhost", port, true, AF_UNSPEC);
	ASSERT_NE(serverSide, UNASSIGNED_SYSTEM_ADDRESS) << "Connect(\"localhost\", AF_UNSPEC) did not fall back to 127.0.0.1";
	EXPECT_EQ(serverSide.GetIPVersion(), 4);
	EXPECT_STREQ(serverSide.ToString(false), "127.0.0.1");
	EXPECT_EQ(serverSide.GetPort(), port);
	EXPECT_EQ(WaitForIncoming(1).size(), 1u);
}

TEST_F(TcpInterfaceFamilyLive, NonBlockingUnspecConnectByHostNameTriesEveryAddress)
{
	if (!LocalhostResolvesToIpv6Loopback())
		GTEST_SKIP() << "\"localhost\" has no ::1 answer on this host";
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0));

	client->Connect("localhost", port, false, AF_UNSPEC);
	SystemAddress completed = UNASSIGNED_SYSTEM_ADDRESS, failed = UNASSIGNED_SYSTEM_ADDRESS;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && completed == UNASSIGNED_SYSTEM_ADDRESS && failed == UNASSIGNED_SYSTEM_ADDRESS)
	{
		completed = client->HasCompletedConnectionAttempt();
		failed = client->HasFailedConnectionAttempt();
		if (completed == UNASSIGNED_SYSTEM_ADDRESS && failed == UNASSIGNED_SYSTEM_ADDRESS)
			RakSleep(30);
	}
	ASSERT_EQ(failed, UNASSIGNED_SYSTEM_ADDRESS) << "Connect(\"localhost\", AF_UNSPEC) did not fall back to 127.0.0.1";
	ASSERT_NE(completed, UNASSIGNED_SYSTEM_ADDRESS) << "the connection attempt never completed";
	EXPECT_EQ(completed.GetIPVersion(), 4);
	EXPECT_STREQ(completed.ToString(false), "127.0.0.1");
	EXPECT_EQ(completed.GetPort(), port);
	EXPECT_EQ(WaitForIncoming(1).size(), 1u);
}

// With every client slot taken, Connect must fail without touching memory past
// the slot array (it used to write remoteClients[maxConnections]).
TEST_F(TcpInterfaceFamilyLive, ConnectFailsCleanlyWhenEverySlotIsInUse)
{
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0, 1)); // one slot
	ASSERT_NE(client->Connect("127.0.0.1", port, true, AF_INET), UNASSIGNED_SYSTEM_ADDRESS);

	EXPECT_EQ(client->Connect("127.0.0.1", port, true, AF_INET), UNASSIGNED_SYSTEM_ADDRESS);
	EXPECT_EQ(client->Connect("127.0.0.1", port, false, AF_INET), UNASSIGNED_SYSTEM_ADDRESS);
	EXPECT_EQ(client->GetConnectionCount(), 1u);
	EXPECT_EQ(WaitForIncoming(1).size(), 1u);
}

// Connections opened and closed from the user thread while another thread
// broadcasts and the update threads read and write: a closed connection's slot
// is reused at once, while the update thread may still hold the old socket.
// Only the update thread closes sockets, so it never reads, writes or selects
// on a descriptor that was closed or handed to another connection (TSan
// reported exactly that before). The connections must keep working throughout.
TEST_F(TcpInterfaceFamilyLive, ConnectCloseLoopWhileSending)
{
	ASSERT_TRUE(server->Start(0, 8, 8, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0, 2)); // two slots: every Connect reuses a just-closed one

	// Joined at scope exit, before TearDown stops the server, even if an assertion fails.
	struct Sender
	{
		explicit Sender(TCPInterface *tcp)
		    : thread([this, tcp]
		             {
			const std::string payload(32, 'b');
			while (!stop.load())
			{
				tcp->Send(payload.data(), (unsigned) payload.size(), UNASSIGNED_SYSTEM_ADDRESS, true);
				RakSleep(1);
			} })
		{
		}
		~Sender()
		{
			stop.store(true);
			thread.join();
		}
		std::atomic<bool> stop{false};
		std::thread thread;
	} sender(server.get());

	auto drain = [](TCPInterface *tcp)
	{
		for (Packet *p = tcp->Receive(); p != nullptr; p = tcp->Receive())
			tcp->DeallocatePacket(p);
		while (tcp->HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS)
		{
		}
	};

	const char hello[] = "hello";
	for (int round = 0; round < 60; ++round)
	{
		const SystemAddress serverSide = client->Connect("127.0.0.1", port, true, AF_INET);
		ASSERT_NE(serverSide, UNASSIGNED_SYSTEM_ADDRESS) << "round " << round;
		client->Send(hello, sizeof(hello), serverSide, false);
		// Wait for the server to accept it, so the churn never fills its slots
		// (a full server refuses connections by design).
		SystemAddress accepted = UNASSIGNED_SYSTEM_ADDRESS;
		const TimeMS acceptStart = GetTimeMS();
		while (accepted == UNASSIGNED_SYSTEM_ADDRESS && GetTimeMS() - acceptStart < 5000)
		{
			accepted = server->HasNewIncomingConnection();
			if (accepted == UNASSIGNED_SYSTEM_ADDRESS)
				RakSleep(1);
		}
		ASSERT_NE(accepted, UNASSIGNED_SYSTEM_ADDRESS) << "round " << round << ": the server never accepted";
		// The server closes half of the connections itself, racing its own update thread.
		if (round % 2 == 1)
			server->CloseConnection(accepted);
		drain(server.get());
		drain(client.get());
		client->CloseConnection(serverSide);
	}

	// After the churn, a new connection carries data both ways.
	const SystemAddress serverSide = client->Connect("127.0.0.1", port, true, AF_INET);
	ASSERT_NE(serverSide, UNASSIGNED_SYSTEM_ADDRESS);
	const std::string marker = "marker-after-churn";
	client->Send(marker.data(), (unsigned) marker.size(), serverSide, false);
	bool serverGotMarker = false, clientGotBroadcast = false;
	// TCP may split the marker over several packets: look for it in each connection's byte stream.
	std::map<std::string, std::string> serverStreams;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && !(serverGotMarker && clientGotBroadcast))
	{
		for (Packet *p = server->Receive(); p != nullptr; p = server->Receive())
		{
			std::string &stream = serverStreams[p->systemAddress.ToString(true)];
			stream.append((const char *) p->data, p->length);
			if (stream.find(marker) != std::string::npos)
				serverGotMarker = true;
			server->DeallocatePacket(p);
		}
		for (Packet *p = client->Receive(); p != nullptr; p = client->Receive())
		{
			if (p->systemAddress == serverSide && p->length > 0)
				clientGotBroadcast = true;
			client->DeallocatePacket(p);
		}
		RakSleep(30);
	}
	EXPECT_TRUE(serverGotMarker) << "the server never received the marker";
	EXPECT_TRUE(clientGotBroadcast) << "the client never received the server's broadcast";
}

// Stop() right after a non-blocking Connect: the connection thread writes the
// client slots, so Stop() must wait for it before freeing them (ASan/TSan
// caught the use after free), and must abort a connect that would block for
// the whole TCP timeout. 192.0.2.1 (TEST-NET-1) is never answered: the connect
// blocks where it is routed, and fails at once where it is not. The loopback
// listener completes the handshake in its backlog without ever accepting.
TEST_F(TcpInterfaceFamilyLive, StopRightAfterNonBlockingConnect)
{
	ASSERT_TRUE(server->Start(0, 1, 1, -99999, AF_INET, "127.0.0.1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	for (int round = 0; round < 10; ++round)
	{
		ASSERT_TRUE(client->Start(0, 0, 2));
		client->Connect("192.0.2.1", 9, false, AF_INET);
		client->Connect("127.0.0.1", port, false, AF_INET);
		const TimeMS start = GetTimeMS();
		client->Stop();
		EXPECT_LT(GetTimeMS() - start, 5000u) << "round " << round << ": Stop() waited for a blocked connect";
		EXPECT_FALSE(client->WasStarted());
	}
}

class TcpInterfaceDualStackLive : public TcpInterfaceFamilyLive
{
protected:
	void SetUp() override
	{
		TcpInterfaceFamilyLive::SetUp();
		MAFIANET_SKIP_WITHOUT_IPV6(HasIpv6Loopback() ? 1 : 0);
	}
};

// An AF_UNSPEC listener is one dual-stack socket, whatever order the resolver
// lists the wildcard addresses in: IPv4 and IPv6 clients both get in, and the
// IPv4 one is reported as a plain IPv4 address, not ::ffff:127.0.0.1.
TEST_F(TcpInterfaceDualStackLive, UnspecListenerAcceptsIpv4AndIpv6Clients)
{
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_UNSPEC));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	if (server->ListenFamily() != AF_INET6)
	{
		// ::1 works but the host refuses dual-stack sockets (OpenBSD).
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "AF_UNSPEC listener is not dual-stack but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no dual-stack sockets on this host";
	}
	ASSERT_TRUE(client->Start(0, 0));
	ASSERT_TRUE(client6->Start(0, 0));

	const SystemAddress via4 = client->Connect("127.0.0.1", port, true, AF_INET);
	ASSERT_NE(via4, UNASSIGNED_SYSTEM_ADDRESS) << "IPv4 client could not connect";
	const SystemAddress via6 = client6->Connect("::1", port, true, AF_INET6);
	ASSERT_NE(via6, UNASSIGNED_SYSTEM_ADDRESS) << "IPv6 client could not connect";

	const std::vector<SystemAddress> incoming = WaitForIncoming(2);
	ASSERT_EQ(incoming.size(), 2u) << "the server did not report both clients";
	int ipv4Clients = 0, ipv6Clients = 0;
	for (const SystemAddress &a : incoming)
	{
		EXPECT_FALSE(a.IsIPv4Mapped()) << a.ToString(true);
		if (a.GetIPVersion() == 4)
		{
			++ipv4Clients;
			EXPECT_STREQ(a.ToString(false), "127.0.0.1");
		}
		else
		{
			++ipv6Clients;
			EXPECT_STREQ(a.ToString(false), "::1");
		}
	}
	EXPECT_EQ(ipv4Clients, 1);
	EXPECT_EQ(ipv6Clients, 1);
}
