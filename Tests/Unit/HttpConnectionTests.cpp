/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// HTTPConnection and HTTPConnection2 request queues (std::deque / std::vector), driven without a
// network: nothing here starts the TCPInterface, so no socket or thread exists.

#include <gtest/gtest.h>

#if defined(MAFIANET_TESTS_HAVE_EXTRAS)

#include "mafianet/http_connection.h"
#include "mafianet/http_connection2.h"
#include "mafianet/tcp_interface.h"

#include <cstring>
#include <string>

using namespace MafiaNet;

TEST(HttpConnection, QueuedCommandsKeepTheirOrder)
{
	HTTPConnection http;
	http.Post("/first", "a=1");
	http.Get("/second");
	ASSERT_EQ(http.outgoingCommand.size(), 2u);
	EXPECT_STREQ(http.outgoingCommand.front().remotePath.c_str(), "/first");
	EXPECT_TRUE(http.outgoingCommand.front().isPost);
	EXPECT_STREQ(http.outgoingCommand.back().remotePath.c_str(), "/second");
	EXPECT_FALSE(http.outgoingCommand.back().isPost);
	EXPECT_FALSE(http.HasRead());
	EXPECT_EQ(http.Read(), std::string());
}

TEST(HttpConnection, BadResponseIsConsumedEvenWithoutADataPointer)
{
	TCPInterface tcp; // never started
	HTTPConnection http;
	http.Init(&tcp, "example.invalid");

	std::string response = "HTTP/1.0 404 Not Found\r\n\r\n";
	Packet packet{};
	packet.data = reinterpret_cast<unsigned char *>(response.data());
	packet.length = static_cast<unsigned int>(response.size());
	packet.systemAddress = http.GetServerAddress();
	http.ProcessTCPPacket(&packet);
	http.ProcessTCPPacket(&packet);

	int code = 0;
	ASSERT_TRUE(http.HasBadResponse(&code, nullptr));
	EXPECT_EQ(code, 404);
	std::string data;
	ASSERT_TRUE(http.HasBadResponse(nullptr, &data));
	EXPECT_EQ(std::strncmp(data.c_str(), "HTTP/1.0 404", 12), 0);
	EXPECT_FALSE(http.HasBadResponse(&code, &data));
}

TEST(HttpConnection2, RequestToAnUnconnectedHostWaitsInThePendingQueue)
{
	TCPInterface tcp; // never started: Connect() is refused, the request stays pending
	HTTPConnection2 http;
	tcp.AttachPlugin(&http);
	EXPECT_FALSE(http.IsBusy());
	EXPECT_TRUE(http.TransmitRequest("GET / HTTP/1.0\r\n\r\n", "127.0.0.1", 80));
	EXPECT_TRUE(http.IsBusy());
	EXPECT_FALSE(http.HasResponse());
	tcp.DetachPlugin(&http);
}

TEST(HttpConnection2, FailedConnectionDropsEveryRequestForThatHost)
{
	// Two requests wait for the same host; the connection attempt fails. RemovePendingRequest
	// used to advance its index after each removal too, so it skipped the second request.
	TCPInterface tcp; // never started: Connect() is refused, both requests stay pending
	HTTPConnection2 http;
	tcp.AttachPlugin(&http);
	ASSERT_TRUE(http.TransmitRequest("GET /a HTTP/1.0\r\n\r\n", "127.0.0.1", 80));
	ASSERT_TRUE(http.TransmitRequest("GET /b HTTP/1.0\r\n\r\n", "127.0.0.1", 80));
	ASSERT_TRUE(http.IsBusy());

	Packet failed{};
	failed.systemAddress = SystemAddress("127.0.0.1", 80);
	http.OnFailedConnectionAttempt(&failed, FCAR_CONNECTION_ATTEMPT_FAILED);
	EXPECT_FALSE(http.IsBusy());
	tcp.DetachPlugin(&http);
}

#endif
