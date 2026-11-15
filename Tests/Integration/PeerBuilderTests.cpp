/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/ds_list.h"
#include "mafianet/peer_interface.h"
#include "mafianet/peer_handle.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/get_time.h"

#include <cstring> // std::memcmp, std::memcpy
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

// Shared incoming password, exercised on both the server (SetIncomingPassword)
// and the client (Connect password argument) sides.
static const char *PEERBUILDER_PASSWORD = "hunter2";

// --- Server builder replaces the manual 3-call sequence; client connects.
// Then: startup failure surfaces the underlying StartupResult. Every server
// binds an OS-assigned port (.port(0)); the clash phase re-binds the port its
// holder was given.
TEST(PeerBuilder, ServerClientConnectAndPortClash)
{
	// --- 1. Server builder replaces the manual 3-call sequence; client connects ---
	{
		auto serverResult = Peer::server()
		                        .port(0)
		                        .host("127.0.0.1")
		                        .max_connections(4)
		                        .incoming_password(PEERBUILDER_PASSWORD)
		                        .start();
		ASSERT_TRUE(serverResult)
		    << "server builder start() failed: StartupResult " << (int) serverResult.error().startup;
		ASSERT_TRUE(serverResult->get()->IsActive()) << "server is not active after start()";
		// The builder must have applied SetMaximumIncomingConnections, not just Startup.
		ASSERT_EQ(serverResult->get()->GetMaximumIncomingConnections(), 4)
		    << "server incoming-connection cap not set by builder";
		const unsigned short serverPort = serverResult->get()->GetInternalID().GetPort();
		ASSERT_NE(serverPort, 0) << "server has no bound port";

		auto clientResult = Peer::client()
		                        .max_connections(1)
		                        .connect("127.0.0.1", serverPort, PEERBUILDER_PASSWORD)
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
		// server/client are RAII Peers: they DestroyInstance here.
	}

	// --- 2. Startup failure surfaces the underlying StartupResult ---
	{
		// Hold an OS-assigned port so the second server cannot bind it.
		auto holder = Peer::server()
		                  .port(0)
		                  .max_connections(1)
		                  .start();
		ASSERT_TRUE(holder) << "port-holder server failed to start";
		const unsigned short heldPort = holder->get()->GetInternalID().GetPort();
		ASSERT_NE(heldPort, 0) << "port-holder server has no bound port";

		auto clash = Peer::server()
		                 .port(heldPort)
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
	// without any network round-trip, so the remote port is never used.
	auto badClient = Peer::client()
	                     .max_connections(1)
	                     .connect("!!!not_a_valid_host!!!", 1)
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

// --- encryption() is applied before Startup, and both builders honour it ---
// A Plaintext server accepts a Plaintext client and refuses a default (Required) one,
// which then hears ID_OUR_SYSTEM_REQUIRES_SECURITY.
TEST(PeerBuilder, EncryptionModeReachesThePeerAndMustMatch)
{
	auto serverResult = Peer::server().port(0).host("127.0.0.1").max_connections(4).encryption(EncryptionMode::Plaintext).start();
	ASSERT_TRUE(serverResult) << "StartupResult " << (int) serverResult.error().startup;
	Peer server = std::move(*serverResult);
	EXPECT_EQ(EncryptionMode::Plaintext, server->GetEncryptionMode());
	const unsigned short serverPort = server->GetInternalID().GetPort();
	ASSERT_NE(serverPort, 0);

	auto plainResult = Peer::client().max_connections(1).encryption(EncryptionMode::Plaintext).connect("127.0.0.1", serverPort).start();
	ASSERT_TRUE(plainResult);
	Peer plain = std::move(*plainResult);
	EXPECT_EQ(EncryptionMode::Plaintext, plain->GetEncryptionMode());

	auto strictResult = Peer::client().max_connections(1).connect("127.0.0.1", serverPort).start();
	ASSERT_TRUE(strictResult);
	Peer strict = std::move(*strictResult);
	EXPECT_EQ(EncryptionMode::Required, strict->GetEncryptionMode());

	bool plainAccepted = false;
	bool strictRefused = false;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !(plainAccepted && strictRefused))
	{
		for (auto pkt : server.incoming())
			(void) pkt;
		for (auto pkt : plain.incoming())
			plainAccepted = plainAccepted || pkt.id() == ID_CONNECTION_REQUEST_ACCEPTED;
		for (auto pkt : strict.incoming())
		{
			ASSERT_NE(ID_CONNECTION_REQUEST_ACCEPTED, pkt.id()) << "a Required client connected to a Plaintext server";
			strictRefused = strictRefused || pkt.id() == ID_OUR_SYSTEM_REQUIRES_SECURITY;
		}
		RakSleep(30);
	}
	EXPECT_TRUE(plainAccepted) << "the Plaintext client was not accepted within 10s";
	EXPECT_TRUE(strictRefused) << "the Required client did not hear ID_OUR_SYSTEM_REQUIRES_SECURITY within 10s";
}

// --- identity_key() and pin_server_identity() ---
// A client pinning the server's identity connects and reads the key back; one pinning another key hears
// ID_PUBLIC_KEY_MISMATCH and never connects. A malformed key fails start().
TEST(PeerBuilder, IdentityKeyAndPinReachThePeers)
{
	uint8_t publicKey[kIdentityPublicKeyBytes], secretKey[kIdentitySecretKeyBytes];
	GenerateIdentityKeyPair(publicKey, secretKey);
	uint8_t otherPublic[kIdentityPublicKeyBytes], otherSecret[kIdentitySecretKeyBytes];
	GenerateIdentityKeyPair(otherPublic, otherSecret);

	uint8_t malformed[kIdentitySecretKeyBytes];
	std::memcpy(malformed, secretKey, sizeof(malformed));
	malformed[50] ^= 0x01;
	auto badResult = Peer::server().port(0).host("127.0.0.1").identity_key(malformed).start();
	ASSERT_FALSE(badResult);
	EXPECT_EQ(STARTUP_OTHER_FAILURE, badResult.error().startup);

	auto serverResult = Peer::server().port(0).host("127.0.0.1").max_connections(4).identity_key(secretKey).start();
	ASSERT_TRUE(serverResult) << "StartupResult " << (int) serverResult.error().startup;
	Peer server = std::move(*serverResult);
	uint8_t readBack[kIdentityPublicKeyBytes];
	ASSERT_TRUE(server->GetIdentityPublicKey(readBack));
	EXPECT_EQ(0, std::memcmp(publicKey, readBack, sizeof(readBack)));
	const unsigned short serverPort = server->GetInternalID().GetPort();
	ASSERT_NE(serverPort, 0);

	auto goodResult = Peer::client().max_connections(1).pin_server_identity(publicKey).connect("127.0.0.1", serverPort).start();
	ASSERT_TRUE(goodResult);
	Peer good = std::move(*goodResult);
	auto wrongResult = Peer::client().max_connections(1).pin_server_identity(otherPublic).connect("127.0.0.1", serverPort).start();
	ASSERT_TRUE(wrongResult);
	Peer wrong = std::move(*wrongResult);

	bool goodAccepted = false;
	bool wrongRefused = false;
	SystemAddress serverAddress;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !(goodAccepted && wrongRefused))
	{
		for (auto pkt : server.incoming())
			(void) pkt;
		for (auto pkt : good.incoming())
		{
			ASSERT_NE(ID_PUBLIC_KEY_MISMATCH, pkt.id()) << "the right pin was refused";
			if (pkt.id() == ID_CONNECTION_REQUEST_ACCEPTED)
			{
				goodAccepted = true;
				serverAddress = pkt->systemAddress;
			}
		}
		for (auto pkt : wrong.incoming())
		{
			ASSERT_NE(ID_CONNECTION_REQUEST_ACCEPTED, pkt.id()) << "a client pinning another key connected";
			wrongRefused = wrongRefused || pkt.id() == ID_PUBLIC_KEY_MISMATCH;
		}
		RakSleep(30);
	}
	ASSERT_TRUE(goodAccepted) << "the pinned client was not accepted within 10s";
	EXPECT_TRUE(wrongRefused) << "the wrongly pinned client did not hear ID_PUBLIC_KEY_MISMATCH within 10s";
	uint8_t remote[kIdentityPublicKeyBytes];
	ASSERT_TRUE(good->GetRemoteIdentityKey(serverAddress, remote));
	EXPECT_EQ(0, std::memcmp(publicKey, remote, sizeof(remote)));
	SecureZero(secretKey, sizeof(secretKey));
	SecureZero(otherSecret, sizeof(otherSecret));
}
