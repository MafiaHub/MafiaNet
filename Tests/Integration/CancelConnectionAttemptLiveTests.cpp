/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  CancelConnectionAttempt() from the application thread while the threaded
 *  network threads of both peers complete the server's acceptance. The cancel
 *  must win wherever it lands: the client never reports the connection, and
 *  the server ends up with nothing connected.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"

#include "mafianet/message_identifiers.h"
#include "mafianet/peer_handle.h"
#include "mafianet/sleep.h"

using namespace MafiaNet;

namespace
{
struct Seen
{
	bool request = false;
	RakNetGUID guid;
	int incoming = 0;
	int closed = 0;
	int abandoned = 0;
};

void PumpServer(Peer &server, Seen &seen)
{
	while (PacketPtr packet = server.receive())
	{
		switch (packet->data[0])
		{
		case ID_SESSION_CONFIG_REQUEST:
			seen.request = true;
			seen.guid = packet->guid;
			break;
		case ID_NEW_INCOMING_CONNECTION:
			seen.incoming++;
			break;
		case ID_DISCONNECTION_NOTIFICATION:
		case ID_CONNECTION_LOST:
			seen.closed++;
			break;
		case ID_SESSION_CONFIG_ABANDONED:
			seen.abandoned++;
			break;
		default:
			break;
		}
	}
}
} // namespace

// Each round races the cancel against the acceptance differently: before it, right behind it, or after the client's
// network thread queued it while the application was not reading.
TEST(CancelConnectionAttemptLive, CancelledAttemptNeverConnectsWhileTheServerAccepts)
{
	auto started = Peer::server().capacity({4, 4}).start();
	ASSERT_TRUE(started);
	Peer server = std::move(*started);
	const unsigned short port = TestHelpers::BoundPort(server.get());
	ASSERT_NE(port, 0);

	for (int round = 0; round < 12; ++round)
	{
		SCOPED_TRACE(round);
		Peer client;
		ASSERT_NE(TestHelpers::StartOnLoopback(client.get(), 1), 0);
		ASSERT_EQ(client->Connect("127.0.0.1", port, nullptr, 0), CONNECTION_ATTEMPT_STARTED);

		Seen seen;
		TimeMS start = GetTimeMS();
		while (!seen.request && GetTimeMS() - start < 5000)
		{
			PumpServer(server, seen);
			RakSleep(5);
		}
		ASSERT_TRUE(seen.request);

		const SystemAddress target("127.0.0.1", port);
		switch (round % 3)
		{
		case 0:
			client->CancelConnectionAttempt(target);
			server->AcceptSession(seen.guid, nullptr, 0);
			break;
		case 1:
			server->AcceptSession(seen.guid, nullptr, 0);
			client->CancelConnectionAttempt(target);
			break;
		default:
			server->AcceptSession(seen.guid, nullptr, 0);
			start = GetTimeMS();
			while (client->GetConnectionState(target) != IS_CONNECTED && GetTimeMS() - start < 5000)
				RakSleep(5);
			client->CancelConnectionAttempt(target);
			break;
		}

		bool clientConnected = false;
		start = GetTimeMS();
		while (GetTimeMS() - start < 5000)
		{
			PumpServer(server, seen);
			while (PacketPtr packet = client.receive())
				clientConnected = clientConnected || packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED;
			const bool serverDone = seen.abandoned == 1 || (seen.incoming == 1 && seen.closed == 1);
			if (serverDone && server->NumberOfConnections() == 0 && client->NumberOfConnections() == 0)
				break;
			RakSleep(10);
		}
		EXPECT_FALSE(clientConnected) << "the cancelled attempt reported a connection";
		EXPECT_TRUE(seen.abandoned == 1 || (seen.incoming == 1 && seen.closed == 1))
		    << "abandoned " << seen.abandoned << " incoming " << seen.incoming << " closed " << seen.closed;
		EXPECT_EQ(server->NumberOfConnections(), 0u);
		EXPECT_EQ(client->NumberOfConnections(), 0u);
	}
}
