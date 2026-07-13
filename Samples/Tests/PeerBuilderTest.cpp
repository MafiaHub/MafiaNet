/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "PeerBuilderTest.h"

#include <utility> // std::move

// A distinct port so a full-suite run does not collide with other tests.
static const unsigned short PEERBUILDER_TEST_PORT = 61016;

// Shared incoming password, exercised on both the server (SetIncomingPassword)
// and the client (Connect password argument) sides.
static const char *PEERBUILDER_PASSWORD = "hunter2";

int PeerBuilderTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	// --- 1. Server builder replaces the manual 3-call sequence; client connects ---
	{
		auto serverResult = Peer::server()
			.port(PEERBUILDER_TEST_PORT)
			.max_connections(4)
			.incoming_password(PEERBUILDER_PASSWORD)
			.start();
		if (!serverResult)
		{
			if (isVerbose) printf("server builder start() failed: StartupResult %d\n", (int) serverResult.error().startup);
			return 1;
		}
		if (!serverResult->get()->IsActive())
		{
			if (isVerbose) printf("server is not active after start()\n");
			return 2;
		}
		// The builder must have applied SetMaximumIncomingConnections, not just Startup.
		if (serverResult->get()->GetMaximumIncomingConnections() != 4)
		{
			if (isVerbose) printf("server incoming-connection cap not set by builder\n");
			return 3;
		}

		auto clientResult = Peer::client()
			.max_connections(1)
			.connect("127.0.0.1", PEERBUILDER_TEST_PORT, PEERBUILDER_PASSWORD)
			.start();
		if (!clientResult)
		{
			if (isVerbose) printf("client builder start() failed: stage %d\n", (int) clientResult.error().stage);
			return 4;
		}

		// Move the peers out of their Results to prove Result<Peer> is move-only
		// and the handle survives the transfer.
		Peer server = std::move(*serverResult);
		Peer client = std::move(*clientResult);

		bool clientAccepted = false;
		bool serverGotIncoming = false;
		TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 10000 && !(clientAccepted && serverGotIncoming))
		{
			for (auto pkt : server.incoming())
			{
				if (pkt.id() == ID_NEW_INCOMING_CONNECTION)
					serverGotIncoming = true;
			}
			for (auto pkt : client.incoming())
			{
				if (pkt.id() == ID_CONNECTION_REQUEST_ACCEPTED)
					clientAccepted = true;
				else if (pkt.id() == ID_CONNECTION_ATTEMPT_FAILED ||
				         pkt.id() == ID_INVALID_PASSWORD)
				{
					if (isVerbose) printf("client connection rejected (id %d)\n", pkt.id());
					return 5;
				}
			}
			RakSleep(30);
		}
		if (!clientAccepted)
		{
			if (isVerbose) printf("client was not accepted within 10s\n");
			return 6;
		}
		if (!serverGotIncoming)
		{
			if (isVerbose) printf("server did not observe the incoming connection within 10s\n");
			return 7;
		}
		// server/client are RAII Peers: they DestroyInstance here, freeing the
		// port before the next sub-test reuses it.
	}

	// --- 2. Startup failure surfaces the underlying StartupResult ---
	{
		// Hold the port so the second server cannot bind it.
		auto holder = Peer::server()
			.port(PEERBUILDER_TEST_PORT)
			.max_connections(1)
			.start();
		if (!holder)
		{
			if (isVerbose) printf("port-holder server failed to start\n");
			return 10;
		}

		auto clash = Peer::server()
			.port(PEERBUILDER_TEST_PORT)
			.max_connections(1)
			.start();
		if (clash)
		{
			if (isVerbose) printf("second server unexpectedly bound an in-use port\n");
			return 11;
		}
		if (clash.error().stage != PeerStage::Startup)
		{
			if (isVerbose) printf("port-clash failure did not report PeerStage::Startup\n");
			return 12;
		}
		// The engine enum is preserved, not collapsed to a bool.
		if (clash.error().startup == RAKNET_STARTED)
		{
			if (isVerbose) printf("port-clash failure carries RAKNET_STARTED (enum was lost)\n");
			return 13;
		}
	}

	// --- 3. Connect failure surfaces the underlying ConnectionAttemptResult ---
	{
		// A syntactically invalid host fails address resolution synchronously
		// (getaddrinfo rejects it), so Connect returns CANNOT_RESOLVE_DOMAIN_NAME
		// without any network round-trip.
		auto badClient = Peer::client()
			.max_connections(1)
			.connect("!!!not_a_valid_host!!!", PEERBUILDER_TEST_PORT)
			.start();
		if (badClient)
		{
			if (isVerbose) printf("client unexpectedly connected to an invalid host\n");
			return 20;
		}
		if (badClient.error().stage != PeerStage::Connect)
		{
			if (isVerbose) printf("connect failure did not report PeerStage::Connect (stage %d)\n", (int) badClient.error().stage);
			return 21;
		}
		if (badClient.error().connect == CONNECTION_ATTEMPT_STARTED)
		{
			if (isVerbose) printf("connect failure carries CONNECTION_ATTEMPT_STARTED (enum was lost)\n");
			return 22;
		}
	}

	// --- 4. Client builder without a target performs Startup only ---
	{
		auto idle = Peer::client().max_connections(1).start();
		if (!idle)
		{
			if (isVerbose) printf("startup-only client builder failed: StartupResult %d\n", (int) idle.error().startup);
			return 30;
		}
		if (!idle->get()->IsActive())
		{
			if (isVerbose) printf("startup-only client is not active\n");
			return 31;
		}
	}

	return 0;
}

PeerBuilderTest::PeerBuilderTest(void)
{
}

PeerBuilderTest::~PeerBuilderTest(void)
{
}

RakString PeerBuilderTest::GetTestName(void)
{
	return "PeerBuilderTest";
}

RakString PeerBuilderTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "Server builder start() failed";
	case 2:  return "Server not active after start()";
	case 3:  return "Server incoming-connection cap not set";
	case 4:  return "Client builder start() failed";
	case 5:  return "Client connection rejected";
	case 6:  return "Client not accepted in time";
	case 7:  return "Server did not observe incoming connection";
	case 10: return "Port-holder server failed to start";
	case 11: return "Second server bound an in-use port";
	case 12: return "Port clash did not report PeerStage::Startup";
	case 13: return "Port clash lost the StartupResult enum";
	case 20: return "Client connected to an invalid host";
	case 21: return "Connect failure did not report PeerStage::Connect";
	case 22: return "Connect failure lost the ConnectionAttemptResult enum";
	case 30: return "Startup-only client builder failed";
	case 31: return "Startup-only client not active";
	default: return "Undefined error";
	}
}

void PeerBuilderTest::DestroyPeers(void)
{
	// Peers in this test are RAII MafiaNet::Peer locals scoped to RunTest; they
	// clean themselves up at scope exit, so there is nothing to do here.
}
