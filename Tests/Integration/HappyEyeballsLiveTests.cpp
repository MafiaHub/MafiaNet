/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  CancelConnectionAttempt from the application thread while the threaded
 *  update loop is sending connection requests for the same attempt. The
 *  client is dual-stack and connects to "localhost", which on most hosts
 *  resolves to both ::1 and 127.0.0.1, so the attempt is a Happy Eyeballs
 *  race. The server listens on IPv4 only, so the ::1 candidate never answers
 *  and 127.0.0.1 does. Cancel only flags requests; the network thread is the
 *  one that frees them, so this loop must neither crash nor (under ASan/TSan)
 *  report a use-after-free or a race on a freed request.
 */

#include "SessionTestHelpers.h"
#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"

using namespace MafiaNet;

class HappyEyeballsLive : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server = RakPeerInterface::GetInstance();
		RakSleep(2); // distinct GUID seeds; see the note in TypedSendLiveTests
		client = RakPeerInterface::GetInstance();

		serverPort = TestHelpers::StartOnLoopback(server, 4, 4);
		ASSERT_NE(serverPort, 0);
		unsigned short clientPort = 0;
		MAFIANET_START_DUAL_STACK(clientPort, client, 1, 0);
	}

	void TearDown() override
	{
		for (RakPeerInterface *peer : {client, server})
		{
			if (peer == nullptr)
				continue;
			peer->Shutdown(100);
			RakPeerInterface::DestroyInstance(peer);
		}
		client = server = nullptr;
	}

	static void Drain(RakPeerInterface *peer)
	{
		for (Packet *p = SessionTestHelpers::ReceiveAndAcceptRequests(peer); p != nullptr; p = SessionTestHelpers::ReceiveAndAcceptRequests(peer))
			peer->DeallocatePacket(p);
	}

	RakPeerInterface *server = nullptr;
	RakPeerInterface *client = nullptr;
	unsigned short serverPort = 0;
};

TEST_F(HappyEyeballsLive, CancelWhileTheNetworkThreadSendsIsSafe)
{
	const SystemAddress v6("::1", serverPort);
	const SystemAddress v4("127.0.0.1", serverPort);

	for (int i = 0; i < 200; ++i)
	{
		// 10 ms between requests, so the update thread is sending throughout.
		const ConnectionAttemptResult r = client->Connect("localhost", serverPort, nullptr, 0, nullptr, 0, 6, 10);
		// A cancel that landed after the server answered lets that connection
		// complete; anything else must start afresh.
		ASSERT_TRUE(r == CONNECTION_ATTEMPT_STARTED || r == ALREADY_CONNECTED_TO_ENDPOINT) << "iteration " << i << ": " << (int) r;

		RakSleep((unsigned) (i % 7));
		client->CancelConnectionAttempt(v6);
		client->CancelConnectionAttempt(v4);
		// The cancel is visible as soon as it returns.
		ASSERT_NE(client->GetConnectionState(v6), IS_PENDING) << "iteration " << i;
		ASSERT_NE(client->GetConnectionState(v4), IS_PENDING) << "iteration " << i;

		Drain(client);
		Drain(server);
	}
}
