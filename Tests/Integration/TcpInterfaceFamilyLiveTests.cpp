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

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/socket_includes.h"
#include "mafianet/tcp_interface.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace MafiaNet;

namespace
{
// TCPInterface does not report the port it listens on; this reads it from the
// listening socket, so the test can listen on a port the OS assigns.
class ListeningTcp : public TCPInterface
{
public:
	unsigned short ListenPort() const
	{
		sockaddr_storage local;
		if (!LocalAddress(&local))
			return 0;
		return local.ss_family == AF_INET ? ntohs(((const sockaddr_in *) &local)->sin_port) : ntohs(((const sockaddr_in6 *) &local)->sin6_port);
	}
	int ListenFamily() const
	{
		sockaddr_storage local;
		return LocalAddress(&local) ? local.ss_family : AF_UNSPEC;
	}

private:
	bool LocalAddress(sockaddr_storage *local) const
	{
		memset(local, 0, sizeof(*local));
		socklen_t length = sizeof(*local);
		return listenSocket != 0 && getsockname(listenSocket, (sockaddr *) local, &length) == 0;
	}
};

// Whether a TCP socket can bind ::1 on this host.
bool HasIpv6Loopback()
{
	bool ok = false;
	const int s = (int) socket(AF_INET6, SOCK_STREAM, 0);
	if (s >= 0)
	{
		sockaddr_in6 addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin6_family = AF_INET6;
		addr.sin6_addr.s6_addr[15] = 1; // ::1
		ok = bind(s, (sockaddr *) &addr, sizeof(addr)) == 0;
#ifdef _WIN32
		closesocket(s);
#else
		close(s);
#endif
	}
	return ok;
}
} // namespace

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
