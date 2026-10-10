/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Message framing and connection-notification tests for PacketizedTCP.
 *
 *  PacketizedTCP had no tests. It adds a length prefix on top of TCPInterface's
 *  raw byte stream, which means a received TCP segment may hold part of a
 *  message, exactly one, or several -- and the three protected containers being
 *  migrated to std containers (issue #60) are what hold that state:
 *
 *    connections       SystemAddress -> ByteQueue, the per-peer reassembly buffer
 *    waitingPackets    whole messages already reassembled, waiting for Receive()
 *    the four SystemAddress queues feeding HasNewIncomingConnection and friends
 *
 *  None of it is reachable without a real connection: Send() on an address with
 *  no entry in `connections` returns without sending, and the notification queues
 *  are filled from TCPInterface's own lock-free queues. So these run over
 *  loopback TCP.
 */

#include <gtest/gtest.h>

#include "mafianet/packetized_tcp.h"
#include "mafianet/message_identifiers.h"

#if _RAKNET_SUPPORT_PacketizedTCP == 1

#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "TestHelpers.h"

#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
	class PacketizedTCPFraming : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			server = PacketizedTCP::GetInstance();
			client = PacketizedTCP::GetInstance();
			ASSERT_NE(server, nullptr);
			ASSERT_NE(client, nullptr);

			port = TestHelpers::StartTcpOnAFreePort(*server, 4);
			ASSERT_NE(port, 0) << "could not start a listener on any free TCP port";

			ASSERT_TRUE(client->Start(0, 0));
		}

		void TearDown() override
		{
			// Must survive a failed ASSERT_, so no cleanup is left to the test body.
			if (client)
			{
				DrainAndFree(client);
				client->Stop();
				PacketizedTCP::DestroyInstance(client);
				client = 0;
			}
			if (server)
			{
				DrainAndFree(server);
				server->Stop();
				PacketizedTCP::DestroyInstance(server);
				server = 0;
			}
		}

		static void DrainAndFree(PacketizedTCP *t)
		{
			Packet *p;
			while ((p = t->Receive()) != 0)
				t->DeallocatePacket(p);
		}

		// Connects client to server and waits until each side has observed it.
		// Returns the address the client is known by on the server.
		SystemAddress ConnectAndWait(unsigned int timeoutMS = 10000)
		{
			// A non-blocking Connect always returns UNASSIGNED_SYSTEM_ADDRESS -- it hands
			// the work to a thread and reports the outcome through
			// HasCompletedConnectionAttempt / HasFailedConnectionAttempt. Its return
			// value says nothing, so it is not checked.
			client->Connect("127.0.0.1", port, false);

			SystemAddress completed = UNASSIGNED_SYSTEM_ADDRESS;
			SystemAddress incoming = UNASSIGNED_SYSTEM_ADDRESS;
			const TimeMS start = GetTimeMS();
			while ((completed == UNASSIGNED_SYSTEM_ADDRESS || incoming == UNASSIGNED_SYSTEM_ADDRESS)
				&& GetTimeMS() - start < timeoutMS)
			{
				if (completed == UNASSIGNED_SYSTEM_ADDRESS)
					completed = client->HasCompletedConnectionAttempt();
				if (incoming == UNASSIGNED_SYSTEM_ADDRESS)
					incoming = server->HasNewIncomingConnection();
				if (completed == UNASSIGNED_SYSTEM_ADDRESS || incoming == UNASSIGNED_SYSTEM_ADDRESS)
					RakSleep(10);
			}

			if (completed == UNASSIGNED_SYSTEM_ADDRESS || incoming == UNASSIGNED_SYSTEM_ADDRESS)
				return UNASSIGNED_SYSTEM_ADDRESS;

			return incoming;
		}

		// Collects exactly `count` messages, as strings, or fewer on timeout.
		//
		// ID_DOWNLOAD_PROGRESS is skipped and counted separately. PacketizedTCP
		// emits one of those every 64KB while a message is still incomplete, so a
		// large payload -- or two interleaved ones -- produces them alongside the
		// real messages. They are part of the protocol, not noise: see
		// LargeTransfersEmitWellFormedProgressPackets below.
		std::vector<std::string> ReceiveMessages(PacketizedTCP *t, size_t count,
			unsigned int timeoutMS = 10000)
		{
			std::vector<std::string> out;
			const TimeMS start = GetTimeMS();
			while (out.size() < count && GetTimeMS() - start < timeoutMS)
			{
				Packet *p = t->Receive();
				if (p)
				{
					if (p->length > 0 && p->data[0] == ID_DOWNLOAD_PROGRESS)
						progressPackets++;
					else
						out.push_back(std::string((const char *) p->data, p->length));
					t->DeallocatePacket(p);
				}
				else
					RakSleep(10);
			}
			return out;
		}

		int progressPackets = 0;

		PacketizedTCP *server;
		PacketizedTCP *client;
		unsigned short port;
	};
}

TEST_F(PacketizedTCPFraming, ConnectionIsObservedByBothSides)
{
	const SystemAddress clientOnServer = ConnectAndWait();
	ASSERT_NE(clientOnServer, UNASSIGNED_SYSTEM_ADDRESS) << "connection was never observed";

	// Each notification is handed out once.
	ASSERT_EQ(server->HasNewIncomingConnection(), UNASSIGNED_SYSTEM_ADDRESS);
	ASSERT_EQ(client->HasCompletedConnectionAttempt(), UNASSIGNED_SYSTEM_ADDRESS);

	// And the wrong queue stays empty.
	ASSERT_EQ(client->HasFailedConnectionAttempt(), UNASSIGNED_SYSTEM_ADDRESS);
	ASSERT_EQ(server->HasLostConnection(), UNASSIGNED_SYSTEM_ADDRESS);
}

// One message per Receive, with its exact length -- the length prefix must not
// leak into the payload and the payload must not be truncated.
TEST_F(PacketizedTCPFraming, ASingleMessageArrivesWithItsExactLength)
{
	ASSERT_NE(ConnectAndWait(), UNASSIGNED_SYSTEM_ADDRESS);

	const std::string msg = "hello packetized tcp";
	client->Send(msg.c_str(), (unsigned) msg.size(), UNASSIGNED_SYSTEM_ADDRESS, true);

	const std::vector<std::string> got = ReceiveMessages(server, 1);

	ASSERT_EQ(got.size(), 1u) << "the message never arrived";
	ASSERT_EQ(got[0], msg);
}

// Several small messages sent back to back almost certainly arrive in one TCP
// segment, so the receive path has to split them out of the ByteQueue and queue
// them in waitingPackets. This is the case that pins waitingPackets' FIFO order:
// draining it as a stack would reverse the batch.
TEST_F(PacketizedTCPFraming, ABatchOfMessagesInOneSegmentKeepsItsOrder)
{
	ASSERT_NE(ConnectAndWait(), UNASSIGNED_SYSTEM_ADDRESS);

	std::vector<std::string> sent;
	for (int i = 0; i < 12; i++)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "msg-%02d", i);
		sent.push_back(buf);
	}

	for (size_t i = 0; i < sent.size(); i++)
		client->Send(sent[i].c_str(), (unsigned) sent[i].size(), UNASSIGNED_SYSTEM_ADDRESS, true);

	const std::vector<std::string> got = ReceiveMessages(server, sent.size());

	ASSERT_EQ(got.size(), sent.size()) << "only " << got.size() << " of " << sent.size() << " arrived";
	for (size_t i = 0; i < sent.size(); i++)
		ASSERT_EQ(got[i], sent[i]) << "message " << i << " is out of order or corrupt";
}

// A payload far larger than one segment has to be reassembled from several
// TCPInterface packets inside the per-connection ByteQueue before any of it is
// handed to Receive.
TEST_F(PacketizedTCPFraming, AMessageLargerThanOneSegmentIsReassembled)
{
	ASSERT_NE(ConnectAndWait(), UNASSIGNED_SYSTEM_ADDRESS);

	std::string big;
	big.reserve(200000);
	for (int i = 0; i < 200000; i++)
		big.push_back((char) ('a' + (i % 26)));

	client->Send(big.c_str(), (unsigned) big.size(), UNASSIGNED_SYSTEM_ADDRESS, true);

	const std::vector<std::string> got = ReceiveMessages(server, 1, 20000);

	ASSERT_EQ(got.size(), 1u) << "the large message never completed";
	ASSERT_EQ(got[0].size(), big.size()) << "wrong reassembled length";
	ASSERT_EQ(got[0], big) << "the reassembled payload does not match";
}

// A large message followed by a small one: the small one must not be lost in the
// tail of the ByteQueue, and must not arrive first.
TEST_F(PacketizedTCPFraming, ASmallMessageAfterALargeOneStillArrivesInOrder)
{
	ASSERT_NE(ConnectAndWait(), UNASSIGNED_SYSTEM_ADDRESS);

	std::string big(120000, 'x');
	const std::string small = "tail";

	client->Send(big.c_str(), (unsigned) big.size(), UNASSIGNED_SYSTEM_ADDRESS, true);
	client->Send(small.c_str(), (unsigned) small.size(), UNASSIGNED_SYSTEM_ADDRESS, true);

	const std::vector<std::string> got = ReceiveMessages(server, 2, 20000);

	ASSERT_EQ(got.size(), 2u);
	ASSERT_EQ(got[0].size(), big.size());
	ASSERT_EQ(got[0], big);
	ASSERT_EQ(got[1], small);
}

// Both directions at once, so each side keeps its own reassembly buffer.
TEST_F(PacketizedTCPFraming, BothDirectionsAreFramedIndependently)
{
	const SystemAddress clientOnServer = ConnectAndWait();
	ASSERT_NE(clientOnServer, UNASSIGNED_SYSTEM_ADDRESS);

	const std::string toServer = "client says hello";
	const std::string toClient = "server says hi back";

	client->Send(toServer.c_str(), (unsigned) toServer.size(), UNASSIGNED_SYSTEM_ADDRESS, true);
	server->Send(toClient.c_str(), (unsigned) toClient.size(), clientOnServer, false);

	const std::vector<std::string> atServer = ReceiveMessages(server, 1);
	const std::vector<std::string> atClient = ReceiveMessages(client, 1);

	ASSERT_EQ(atServer.size(), 1u);
	ASSERT_EQ(atServer[0], toServer);
	ASSERT_EQ(atClient.size(), 1u);
	ASSERT_EQ(atClient[0], toClient);
}

// CloseConnection removes the entry from `connections` on the closing side and
// surfaces a lost-connection notification on the other.
TEST_F(PacketizedTCPFraming, ClosingAConnectionIsReportedToThePeer)
{
	const SystemAddress clientOnServer = ConnectAndWait();
	ASSERT_NE(clientOnServer, UNASSIGNED_SYSTEM_ADDRESS);

	server->CloseConnection(clientOnServer);

	SystemAddress lost = UNASSIGNED_SYSTEM_ADDRESS;
	const TimeMS start = GetTimeMS();
	while (lost == UNASSIGNED_SYSTEM_ADDRESS && GetTimeMS() - start < 10000)
	{
		lost = client->HasLostConnection();
		if (lost == UNASSIGNED_SYSTEM_ADDRESS)
			RakSleep(10);
	}

	ASSERT_NE(lost, UNASSIGNED_SYSTEM_ADDRESS) << "the client never saw the connection drop";
	ASSERT_EQ(client->HasLostConnection(), UNASSIGNED_SYSTEM_ADDRESS) << "reported more than once";
}

// Connecting to a port nothing is listening on fills _failedConnectionAttempts,
// which no other test here touches.
TEST_F(PacketizedTCPFraming, AFailedConnectionAttemptIsReported)
{
	const unsigned short deadPort = TestHelpers::FindFreeTcpPort();
	ASSERT_NE(deadPort, 0);
	ASSERT_NE(deadPort, port);

	client->Connect("127.0.0.1", deadPort, false);

	SystemAddress failed = UNASSIGNED_SYSTEM_ADDRESS;
	const TimeMS start = GetTimeMS();
	while (failed == UNASSIGNED_SYSTEM_ADDRESS && GetTimeMS() - start < 15000)
	{
		failed = client->HasFailedConnectionAttempt();
		if (failed == UNASSIGNED_SYSTEM_ADDRESS)
			RakSleep(10);
	}

	ASSERT_NE(failed, UNASSIGNED_SYSTEM_ADDRESS) << "the failed attempt was never reported";
	ASSERT_EQ(client->HasCompletedConnectionAttempt(), UNASSIGNED_SYSTEM_ADDRESS)
		<< "a failed attempt must not also report as completed";
}

// Two clients at once: each needs its own entry in `connections`, keyed by
// address. A single shared buffer would interleave their byte streams and the
// framing would fall apart.
TEST_F(PacketizedTCPFraming, TwoClientsAreFramedSeparately)
{
	ASSERT_NE(ConnectAndWait(), UNASSIGNED_SYSTEM_ADDRESS);

	PacketizedTCP *second = PacketizedTCP::GetInstance();
	ASSERT_NE(second, nullptr);
	ASSERT_TRUE(second->Start(0, 0));

	second->Connect("127.0.0.1", port, false);

	SystemAddress secondOnServer = UNASSIGNED_SYSTEM_ADDRESS;
	const TimeMS start = GetTimeMS();
	while (secondOnServer == UNASSIGNED_SYSTEM_ADDRESS && GetTimeMS() - start < 10000)
	{
		secondOnServer = server->HasNewIncomingConnection();
		if (secondOnServer == UNASSIGNED_SYSTEM_ADDRESS)
			RakSleep(10);
	}

	if (secondOnServer == UNASSIGNED_SYSTEM_ADDRESS)
	{
		DrainAndFree(second);
		second->Stop();
		PacketizedTCP::DestroyInstance(second);
		FAIL() << "the second connection was never observed";
	}

	// Each payload is far bigger than one segment and filled with a distinct
	// pattern, so the server's reads from the two sockets necessarily interleave.
	// A single shared reassembly buffer would splice one client's bytes into the
	// other's message; two small messages would not catch that, because each would
	// already be complete when it was written and would still read back in order.
	std::string fromFirst(180000, 'A');
	std::string fromSecond(180000, 'B');

	client->Send(fromFirst.c_str(), (unsigned) fromFirst.size(), UNASSIGNED_SYSTEM_ADDRESS, true);
	second->Send(fromSecond.c_str(), (unsigned) fromSecond.size(), UNASSIGNED_SYSTEM_ADDRESS, true);

	// Both messages must arrive intact; the order between two sockets is not ours
	// to determine, so match as a set.
	const std::vector<std::string> got = ReceiveMessages(server, 2, 30000);

	DrainAndFree(second);
	second->Stop();
	PacketizedTCP::DestroyInstance(second);

	ASSERT_EQ(got.size(), 2u);
	const bool matched = (got[0] == fromFirst && got[1] == fromSecond)
		|| (got[0] == fromSecond && got[1] == fromFirst);
	ASSERT_TRUE(matched) << "payloads were mixed between the two connections: got "
		<< got[0].size() << " and " << got[1].size() << " bytes";
}

#endif // _RAKNET_SUPPORT_PacketizedTCP
