/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  TCPInterface over a real AF_INET6 loopback connection: a listener bound to
 *  ::1 and a client connecting to "::1" with AF_INET6.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/socket_includes.h"
#include "mafianet/tcp_interface.h"

#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace MafiaNet;

namespace
{
// Asks the OS for a free TCP port on ::1. Returns 0 when ::1 cannot be bound
// (host without IPv6 loopback). TCPInterface has no way to report the port it
// bound, so the test binds a probe socket, reads the port and releases it.
unsigned short ProbeFreeLoopbackPortV6()
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	unsigned short port = 0;
	const int s = (int) socket(AF_INET6, SOCK_STREAM, 0);
	if (s >= 0)
	{
		sockaddr_in6 addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin6_family = AF_INET6;
		addr.sin6_addr.s6_addr[15] = 1; // ::1
		socklen_t len = sizeof(addr);
		if (bind(s, (sockaddr *) &addr, sizeof(addr)) == 0 && getsockname(s, (sockaddr *) &addr, &len) == 0)
			port = ntohs(addr.sin6_port);
#ifdef _WIN32
		closesocket(s);
#else
		close(s);
#endif
	}
#ifdef _WIN32
	WSACleanup();
#endif
	return port;
}
}

class TcpInterfaceIpv6Live : public ::testing::Test
{
protected:
	void SetUp() override
	{
		port = ProbeFreeLoopbackPortV6();
		MAFIANET_SKIP_WITHOUT_IPV6(port);
		server = TCPInterface::GetInstance();
		client = TCPInterface::GetInstance();
	}

	void TearDown() override
	{
		for (TCPInterface *tcp : {client, server})
		{
			if (tcp == nullptr)
				continue;
			tcp->Stop();
			TCPInterface::DestroyInstance(tcp);
		}
		client = server = nullptr;
	}

	TCPInterface *server = nullptr;
	TCPInterface *client = nullptr;
	unsigned short port = 0;
};

TEST_F(TcpInterfaceIpv6Live, EchoOverLoopback)
{
	ASSERT_TRUE(server->Start(port, 4, 4, -99999, AF_INET6, "::1"));
	ASSERT_TRUE(client->Start(0, 0));

	const SystemAddress serverSide = client->Connect("::1", port, true, AF_INET6);
	ASSERT_NE(serverSide, UNASSIGNED_SYSTEM_ADDRESS);

	char sent[64];
	for (int i = 0; i < 64; ++i)
		sent[i] = (char) (i * 3 + 1);
	client->Send(sent, sizeof(sent), serverSide, false);

	bool gotIncoming = false;
	Packet *received = nullptr;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && received == nullptr)
	{
		if (server->HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS)
			gotIncoming = true;
		received = server->Receive();
		if (received == nullptr)
			RakSleep(30);
	}
	ASSERT_NE(received, nullptr) << "server did not receive the 64 bytes";
	// The connection is queued before its data, but may be popped after it.
	while (!gotIncoming && GetTimeMS() - start < 5000)
	{
		gotIncoming = server->HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS;
		if (!gotIncoming)
			RakSleep(30);
	}
	EXPECT_TRUE(gotIncoming) << "the server never reported the incoming connection";
	EXPECT_EQ(received->length, 64u);
	EXPECT_EQ(memcmp(received->data, sent, sizeof(sent)), 0);
	// The peer address the server reports is the IPv6 loopback.
	EXPECT_EQ(received->systemAddress.address.addr6.sin6_family, AF_INET6);
	server->DeallocatePacket(received);
}
