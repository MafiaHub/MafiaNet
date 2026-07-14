/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/DS_List.h"
#include "mafianet/peerinterface.h"
#include "mafianet/PeerHandle.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"

#include <utility> // std::move

using namespace MafiaNet;

/// Verifies the Startup builder for Peer (issue #18):
/// - Peer::server().port(p).max_connections(n).incoming_password(pw).start()
///   replaces the manual Startup + SetMaximumIncomingConnections (+
///   SetIncomingPassword) sequence and a client built with
///   Peer::client().max_connections(1).connect(host, port, pw).start() actually
///   connects over loopback.
/// - The failure path is a Result<Peer> that exposes the underlying engine enum,
///   not a bool: a port clash surfaces the StartupResult (PeerStage::Startup) and
///   an unresolvable target surfaces the ConnectionAttemptResult (PeerStage::Connect).
/// - A client builder with no .connect() target still starts up (Startup only).

// A distinct port so a full-suite run does not collide with other tests.
static const unsigned short PEERBUILDER_TEST_PORT = 61016;

// Shared incoming password, exercised on both the server (SetIncomingPassword)
// and the client (Connect password argument) sides.
static const char *PEERBUILDER_PASSWORD = "hunter2";

// --- Server builder replaces the manual 3-call sequence; client connects.
// Then: startup failure surfaces the underlying StartupResult. Both phases
// bind PEERBUILDER_TEST_PORT, so they stay in one TEST (the first phase's
// peers must have freed the port before the clash phase reuses it).
TEST(PeerBuilder, ServerClientConnectAndPortClash)
{
	// --- 1. Server builder replaces the manual 3-call sequence; client connects ---
	{
		auto serverResult = Peer::server()
			.port(PEERBUILDER_TEST_PORT)
			.max_connections(4)
			.incoming_password(PEERBUILDER_PASSWORD)
			.start();
		ASSERT_TRUE(serverResult)
			<< "server builder start() failed: StartupResult " << (int) serverResult.error().startup;
		ASSERT_TRUE(serverResult->get()->IsActive()) << "server is not active after start()";
		// The builder must have applied SetMaximumIncomingConnections, not just Startup.
		ASSERT_EQ(serverResult->get()->GetMaximumIncomingConnections(), 4)
			<< "server incoming-connection cap not set by builder";

		auto clientResult = Peer::client()
			.max_connections(1)
			.connect("127.0.0.1", PEERBUILDER_TEST_PORT, PEERBUILDER_PASSWORD)
			.start();
		ASSERT_TRUE(clientResult)
			<< "client builder start() failed: stage " << (int) clientResult.error().stage;

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
				else
				{
					ASSERT_FALSE(pkt.id() == ID_CONNECTION_ATTEMPT_FAILED ||
					             pkt.id() == ID_INVALID_PASSWORD)
						<< "client connection rejected (id " << (int) pkt.id() << ")";
				}
			}
			RakSleep(30);
		}
		ASSERT_TRUE(clientAccepted) << "client was not accepted within 10s";
		ASSERT_TRUE(serverGotIncoming) << "server did not observe the incoming connection within 10s";
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
		ASSERT_TRUE(holder) << "port-holder server failed to start";

		auto clash = Peer::server()
			.port(PEERBUILDER_TEST_PORT)
			.max_connections(1)
			.start();
		ASSERT_FALSE(clash) << "second server unexpectedly bound an in-use port";
		ASSERT_EQ(clash.error().stage, PeerStage::Startup)
			<< "port-clash failure did not report PeerStage::Startup";
		// The engine enum is preserved, not collapsed to a bool.
		ASSERT_NE(clash.error().startup, RAKNET_STARTED)
			<< "port-clash failure carries RAKNET_STARTED (enum was lost)";
	}
}

// --- Connect failure surfaces the underlying ConnectionAttemptResult ---
TEST(PeerBuilder, ConnectFailureSurfacesConnectionAttemptResult)
{
	// A syntactically invalid host fails address resolution synchronously
	// (getaddrinfo rejects it), so Connect returns CANNOT_RESOLVE_DOMAIN_NAME
	// without any network round-trip.
	auto badClient = Peer::client()
		.max_connections(1)
		.connect("!!!not_a_valid_host!!!", PEERBUILDER_TEST_PORT)
		.start();
	ASSERT_FALSE(badClient) << "client unexpectedly connected to an invalid host";
	ASSERT_EQ(badClient.error().stage, PeerStage::Connect)
		<< "connect failure did not report PeerStage::Connect (stage "
		<< (int) badClient.error().stage << ")";
	ASSERT_NE(badClient.error().connect, CONNECTION_ATTEMPT_STARTED)
		<< "connect failure carries CONNECTION_ATTEMPT_STARTED (enum was lost)";
}

// --- Client builder without a target performs Startup only ---
TEST(PeerBuilder, ClientBuilderWithoutTargetStartsUpOnly)
{
	auto idle = Peer::client().max_connections(1).start();
	ASSERT_TRUE(idle)
		<< "startup-only client builder failed: StartupResult " << (int) idle.error().startup;
	ASSERT_TRUE(idle->get()->IsActive()) << "startup-only client is not active";
}
