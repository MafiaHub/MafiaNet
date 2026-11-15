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

#include "TcpTestHelpers.h"
#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/tcp_interface.h"

#include <memory>
#include <string>

using namespace MafiaNet;
using TcpTestHelpers::ListeningTcp;

class TcpInterfaceIpv6Live : public ::testing::Test
{
protected:
	void SetUp() override
	{
#ifdef _WIN32
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		MAFIANET_SKIP_WITHOUT_IPV6(TcpTestHelpers::HasIpv6Loopback() ? 1 : 0);
		server.reset(new ListeningTcp);
		client.reset(new TCPInterface);
	}

	void TearDown() override
	{
		if (client)
			client->Stop();
		if (server)
			server->Stop();
		client.reset();
		server.reset();
#ifdef _WIN32
		WSACleanup();
#endif
	}

	std::unique_ptr<ListeningTcp> server;
	std::unique_ptr<TCPInterface> client;
};

TEST_F(TcpInterfaceIpv6Live, EchoOverLoopback)
{
	// Port 0: the OS assigns a free one, so parallel tests never collide.
	ASSERT_TRUE(server->Start(0, 4, 4, -99999, AF_INET6, "::1"));
	const unsigned short port = server->ListenPort();
	ASSERT_NE(port, 0);
	ASSERT_TRUE(client->Start(0, 0));

	const SystemAddress serverSide = client->Connect("::1", port, true, AF_INET6);
	ASSERT_NE(serverSide, UNASSIGNED_SYSTEM_ADDRESS);

	char sent[64];
	for (int i = 0; i < 64; ++i)
		sent[i] = (char) (i * 3 + 1);
	client->Send(sent, sizeof(sent), serverSide, false);

	// TCP may deliver the 64 bytes in several packets.
	std::string received;
	SystemAddress sender = UNASSIGNED_SYSTEM_ADDRESS;
	TcpTestHelpers::ReceiveBytes(server.get(), UNASSIGNED_SYSTEM_ADDRESS, sizeof(sent), &received, &sender);
	ASSERT_EQ(received.size(), sizeof(sent)) << "server did not receive exactly the 64 bytes";
	EXPECT_EQ(received, std::string(sent, sizeof(sent)));
	// The peer address the server reports is the IPv6 loopback.
	EXPECT_EQ(sender.GetIPVersion(), 6);

	// The connection is queued before its data, but may be popped after it.
	bool gotIncoming = false;
	const TimeMS start = GetTimeMS();
	while (!gotIncoming && GetTimeMS() - start < 5000)
	{
		gotIncoming = server->HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS;
		if (!gotIncoming)
			RakSleep(30);
	}
	EXPECT_TRUE(gotIncoming) << "the server never reported the incoming connection";
}
