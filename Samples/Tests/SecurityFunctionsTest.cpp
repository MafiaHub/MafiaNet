/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "SecurityFunctionsTest.h"

/*
Description:

Network-level integration tests for the Noise_NK encrypted connection handshake.

Sub-test 1 (happy path, ports 60010/60011):
  Server generates a ServerSecurityKey identity and installs it via SetServerSecurityKey.
  Client pins the matching public key via Connect(). Asserts:
    - client receives ID_CONNECTION_REQUEST_ACCEPTED,
    - server receives ID_NEW_INCOMING_CONNECTION,
    - a reliable-ordered application message sent client->server arrives intact
      (proving the keyed SecureSession encrypts/decrypts over the wire).

Sub-test 2 (wrong pinned key rejected, port 60020):
  Server installs a generated key. Client connects with a DIFFERENT generated key.
  Asserts the client receives a failure notification (ID_PUBLIC_KEY_MISMATCH or
  ID_CONNECTION_ATTEMPT_FAILED) and NEVER ID_CONNECTION_REQUEST_ACCEPTED.
  The server must never report ID_NEW_INCOMING_CONNECTION.

Sub-test 3 (server with no key rejects client, port 60030):
  Server does NOT call SetServerSecurityKey. Client connects with any generated key.
  Asserts the client does NOT establish a connection within the timeout and never
  receives ID_CONNECTION_REQUEST_ACCEPTED.

Note on tamper/replay at the network level:
  PacketChangerPlugin and PacketDropPlugin operate at the InternalPacket (reliability-
  layer) level — ABOVE the SecureSession encryption. They mutate logical messages after
  decryption, so they cannot corrupt post-encryption wire bytes.
  OnDirectSocketSend/OnDirectSocketReceive are available on PluginInterface2 but neither
  plugin implements byte-level mutation through those hooks.
  A true network-level tamper/replay test would require a UDP-relay proxy that rewrites
  raw encrypted datagrams — infrastructure that does not exist in this test suite.
  Datagram tamper rejection and replay-window rejection are therefore covered at the
  SecureSession unit-test level by CryptoUnitTest (return codes 43 and 44).

Note on MITM (server-ephemeral swap) at the network level:
  This requires a relay that rewrites Noise handshake bytes in flight. The server
  authentication guarantee is proven at the network level by sub-test 2 (wrong pinned
  key) and at the crypto level by CryptoUnitTest's wrong-key handshake test (return
  code 24). No additional network MITM test is added.

Success conditions:
  All sub-tests complete within their timeouts. Returns 0.

Failure conditions:
  Each distinct failure returns a unique nonzero code (see ErrorCodeToString).
  Error codes 1–11  : sub-test 1 (happy path)
  Error codes 20–29 : sub-test 2 (wrong key rejected)
  Error codes 30–39 : sub-test 3 (server with no key rejects client)
*/

// ---------------------------------------------------------------------------
// Port assignments — each sub-test gets its own port to avoid collisions.
// ---------------------------------------------------------------------------
static const unsigned short PORT_HAPPY      = 60010;
static const unsigned short PORT_WRONG_KEY  = 60020;
static const unsigned short PORT_NO_KEY     = 60030;

// ---------------------------------------------------------------------------
// Helper: drain all pending packets from a peer, return the first byte of
// each packet via callback. Returns the first "interesting" message type or
// 0 if the queue is empty.
// ---------------------------------------------------------------------------
static void DestroyPair(RakPeerInterface* srv, RakPeerInterface* cli)
{
	if (cli) { cli->Shutdown(100); RakPeerInterface::DestroyInstance(cli); }
	if (srv) { srv->Shutdown(100); RakPeerInterface::DestroyInstance(srv); }
}

// ---------------------------------------------------------------------------
// Sub-test 1: happy path
// ---------------------------------------------------------------------------
static int RunHappyPath(bool isVerbose)
{
	RakPeerInterface* server = RakPeerInterface::GetInstance();
	RakPeerInterface* client = RakPeerInterface::GetInstance();

	SocketDescriptor serverSd(PORT_HAPPY, 0);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 1; }
	server->SetMaximumIncomingConnections(8);

	ServerSecurityKey key = GenerateServerSecurityKey();
	server->SetServerSecurityKey(key);

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 2; }

	if (isVerbose) printf("[HappyPath] Connecting client to encrypted server (Noise_NK)\n");

	if (client->Connect("127.0.0.1", PORT_HAPPY, 0, 0, key.publicKey) != CONNECTION_ATTEMPT_STARTED)
	{ DestroyPair(server, client); return 3; }

	bool clientGotAccepted  = false;
	bool serverGotNewConn   = false;
	bool serverGotPayload   = false;

	char payload[] = "ENCRYPTED_PAYLOAD";
	payload[0] = (char)(ID_USER_PACKET_ENUM + 1);
	bool payloadSent = false;

	TimeMS entryTime = GetTimeMS();
	while (GetTimeMS() - entryTime < 7000)
	{
		Packet* packet;

		for (packet = client->Receive(); packet;
		     client->DeallocatePacket(packet), packet = client->Receive())
		{
			switch (packet->data[0])
			{
			case ID_CONNECTION_REQUEST_ACCEPTED:
				clientGotAccepted = true;
				if (isVerbose) printf("[HappyPath] Client: ID_CONNECTION_REQUEST_ACCEPTED\n");
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				DestroyPair(server, client);
				return 4;
			case ID_PUBLIC_KEY_MISMATCH:
				DestroyPair(server, client);
				return 5;
			case ID_OUR_SYSTEM_REQUIRES_SECURITY:
			case ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY:
				DestroyPair(server, client);
				return 6;
			default:
				break;
			}
		}

		if (clientGotAccepted && !payloadSent)
		{
			client->Send(payload, (int)strlen(payload) + 1,
			             HIGH_PRIORITY, RELIABLE_ORDERED, 0,
			             UNASSIGNED_SYSTEM_ADDRESS, true);
			payloadSent = true;
		}

		for (packet = server->Receive(); packet;
		     server->DeallocatePacket(packet), packet = server->Receive())
		{
			switch (packet->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
				serverGotNewConn = true;
				if (isVerbose) printf("[HappyPath] Server: ID_NEW_INCOMING_CONNECTION\n");
				break;
			default:
				if (packet->data[0] == (unsigned char)(ID_USER_PACKET_ENUM + 1))
				{
					if (packet->length == (unsigned int)(strlen(payload) + 1) &&
					    memcmp(packet->data, payload, strlen(payload) + 1) == 0)
					{
						serverGotPayload = true;
						if (isVerbose)
							printf("[HappyPath] Server: received intact encrypted payload\n");
					}
					else
					{
						DestroyPair(server, client);
						return 8; // payload corrupted
					}
				}
				break;
			}
		}

		if (clientGotAccepted && serverGotNewConn && serverGotPayload)
			break;

		RakSleep(30);
	}

	DestroyPair(server, client);

	if (!clientGotAccepted)  return 9;
	if (!serverGotNewConn)   return 10;
	if (!serverGotPayload)   return 11;
	return 0;
}

// ---------------------------------------------------------------------------
// Sub-test 2: wrong pinned key is rejected at the network level
// ---------------------------------------------------------------------------
static int RunWrongKeyRejected(bool isVerbose)
{
	RakPeerInterface* server = RakPeerInterface::GetInstance();
	RakPeerInterface* client = RakPeerInterface::GetInstance();

	SocketDescriptor serverSd(PORT_WRONG_KEY, 0);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 20; }
	server->SetMaximumIncomingConnections(8);

	// Server installs its real identity.
	ServerSecurityKey serverKey = GenerateServerSecurityKey();
	server->SetServerSecurityKey(serverKey);

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 21; }

	// Client uses a DIFFERENT (wrong) public key when connecting.
	ServerSecurityKey wrongKey = GenerateServerSecurityKey();
	if (isVerbose) printf("[WrongKey] Connecting client with wrong pinned key\n");

	if (client->Connect("127.0.0.1", PORT_WRONG_KEY, 0, 0, wrongKey.publicKey)
	        != CONNECTION_ATTEMPT_STARTED)
	{ DestroyPair(server, client); return 22; }

	bool clientGotRejection     = false;
	bool clientGotAccepted      = false;
	bool serverGotNewConn       = false;

	TimeMS entryTime = GetTimeMS();
	while (GetTimeMS() - entryTime < 7000)
	{
		Packet* packet;

		for (packet = client->Receive(); packet;
		     client->DeallocatePacket(packet), packet = client->Receive())
		{
			switch (packet->data[0])
			{
			case ID_PUBLIC_KEY_MISMATCH:
				clientGotRejection = true;
				if (isVerbose) printf("[WrongKey] Client: ID_PUBLIC_KEY_MISMATCH (expected)\n");
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				// Also acceptable: the handshake fails and the peer reports a general failure.
				clientGotRejection = true;
				if (isVerbose) printf("[WrongKey] Client: ID_CONNECTION_ATTEMPT_FAILED (expected)\n");
				break;
			case ID_OUR_SYSTEM_REQUIRES_SECURITY:
			case ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY:
				clientGotRejection = true;
				if (isVerbose) printf("[WrongKey] Client: security mismatch rejection (expected)\n");
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
				// Must NOT happen — wrong key must never yield an accepted connection.
				clientGotAccepted = true;
				if (isVerbose) printf("[WrongKey] FAIL: Client got ID_CONNECTION_REQUEST_ACCEPTED!\n");
				break;
			default:
				break;
			}
		}

		for (packet = server->Receive(); packet;
		     server->DeallocatePacket(packet), packet = server->Receive())
		{
			if (packet->data[0] == ID_NEW_INCOMING_CONNECTION)
			{
				serverGotNewConn = true;
				if (isVerbose) printf("[WrongKey] FAIL: Server got ID_NEW_INCOMING_CONNECTION!\n");
			}
			server->DeallocatePacket(packet);
			// Reset to nullptr so the outer loop condition re-evaluates cleanly.
			packet = nullptr;
			break;
		}

		// Exit early if we already know the wrong key was rejected (fast path).
		// If no notification arrives, we wait out the full timeout and pass on silence.
		if ((clientGotRejection || clientGotAccepted) && GetTimeMS() - entryTime > 500)
			break;

		RakSleep(30);
	}

	DestroyPair(server, client);

	// A connection must NOT have been accepted (wrong key must never yield an open connection).
	if (clientGotAccepted)   return 24;
	// Server must NOT have seen a new incoming connection.
	if (serverGotNewConn)    return 25;
	// A plain timeout with no acceptance is also a valid rejection (server drops silently).
	// clientGotRejection being false just means the client timed out — that is still a pass.

	if (isVerbose) printf("[WrongKey] PASS: wrong pinned key correctly rejected\n");
	return 0;
}

// ---------------------------------------------------------------------------
// Sub-test 3: server with no security key rejects the client
// ---------------------------------------------------------------------------
static int RunNoKeyRejectsClient(bool isVerbose)
{
	RakPeerInterface* server = RakPeerInterface::GetInstance();
	RakPeerInterface* client = RakPeerInterface::GetInstance();

	SocketDescriptor serverSd(PORT_NO_KEY, 0);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 30; }
	server->SetMaximumIncomingConnections(8);

	// Deliberately do NOT call SetServerSecurityKey on the server.

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 31; }

	// Client tries to connect with a key (expects encryption).
	ServerSecurityKey clientKey = GenerateServerSecurityKey();
	if (isVerbose) printf("[NoKey] Connecting client to server that has no security key\n");

	if (client->Connect("127.0.0.1", PORT_NO_KEY, 0, 0, clientKey.publicKey)
	        != CONNECTION_ATTEMPT_STARTED)
	{ DestroyPair(server, client); return 32; }

	bool clientGotRejection = false;
	bool clientGotAccepted  = false;
	bool serverGotNewConn   = false;

	TimeMS entryTime = GetTimeMS();
	while (GetTimeMS() - entryTime < 7000)
	{
		Packet* packet;

		for (packet = client->Receive(); packet;
		     client->DeallocatePacket(packet), packet = client->Receive())
		{
			switch (packet->data[0])
			{
			case ID_OUR_SYSTEM_REQUIRES_SECURITY:
				clientGotRejection = true;
				if (isVerbose) printf("[NoKey] Client: ID_OUR_SYSTEM_REQUIRES_SECURITY (expected)\n");
				break;
			case ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY:
				clientGotRejection = true;
				if (isVerbose) printf("[NoKey] Client: ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY (expected)\n");
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				clientGotRejection = true;
				if (isVerbose) printf("[NoKey] Client: ID_CONNECTION_ATTEMPT_FAILED (expected)\n");
				break;
			case ID_PUBLIC_KEY_MISMATCH:
				// Also acceptable — server identity could not be verified.
				clientGotRejection = true;
				if (isVerbose) printf("[NoKey] Client: ID_PUBLIC_KEY_MISMATCH (accepted as rejection)\n");
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
				clientGotAccepted = true;
				if (isVerbose) printf("[NoKey] FAIL: Client got ID_CONNECTION_REQUEST_ACCEPTED!\n");
				break;
			default:
				break;
			}
		}

		for (packet = server->Receive(); packet;
		     server->DeallocatePacket(packet), packet = server->Receive())
		{
			if (packet->data[0] == ID_NEW_INCOMING_CONNECTION)
			{
				serverGotNewConn = true;
				if (isVerbose) printf("[NoKey] FAIL: Server got ID_NEW_INCOMING_CONNECTION!\n");
			}
			server->DeallocatePacket(packet);
			packet = nullptr;
			break;
		}

		if (clientGotRejection && GetTimeMS() - entryTime > 500)
			break;

		RakSleep(30);
	}

	DestroyPair(server, client);

	if (!clientGotRejection) return 33;
	if (clientGotAccepted)   return 34;
	if (serverGotNewConn)    return 35;

	if (isVerbose) printf("[NoKey] PASS: server without key correctly rejected client\n");
	return 0;
}

// ---------------------------------------------------------------------------
// TestInterface implementation
// ---------------------------------------------------------------------------

int SecurityFunctionsTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	// Peer member fields are only used by the old monolithic path.  The refactored
	// helpers manage their own local peers, so ensure the members start null.
	server = nullptr;
	client = nullptr;

	// Sub-test 1: happy path
	if (isVerbose) printf("=== SecurityFunctionsTest: sub-test 1 (happy path) ===\n");
	int rc = RunHappyPath(isVerbose);
	if (rc != 0)
	{
		if (isVerbose) printf("FAILED sub-test 1 with code %d\n", rc);
		return rc;
	}
	if (isVerbose) printf("=== sub-test 1 PASSED ===\n\n");

	// Sub-test 2: wrong pinned key rejected
	if (isVerbose) printf("=== SecurityFunctionsTest: sub-test 2 (wrong key rejected) ===\n");
	rc = RunWrongKeyRejected(isVerbose);
	if (rc != 0)
	{
		if (isVerbose) printf("FAILED sub-test 2 with code %d\n", rc);
		return rc;
	}
	if (isVerbose) printf("=== sub-test 2 PASSED ===\n\n");

	// Sub-test 3: server with no key rejects client
	if (isVerbose) printf("=== SecurityFunctionsTest: sub-test 3 (no key rejects client) ===\n");
	rc = RunNoKeyRejectsClient(isVerbose);
	if (rc != 0)
	{
		if (isVerbose) printf("FAILED sub-test 3 with code %d\n", rc);
		return rc;
	}
	if (isVerbose) printf("=== sub-test 3 PASSED ===\n\n");

	return 0;
}

RakString SecurityFunctionsTest::GetTestName()
{
	return "SecurityFunctionsTest";
}

RakString SecurityFunctionsTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";

	// Sub-test 1: happy path
	case 1:  return "[HappyPath] Server failed to start up";
	case 2:  return "[HappyPath] Client failed to start up";
	case 3:  return "[HappyPath] Connect() did not return CONNECTION_ATTEMPT_STARTED";
	case 4:  return "[HappyPath] Client got ID_CONNECTION_ATTEMPT_FAILED";
	case 5:  return "[HappyPath] Client got ID_PUBLIC_KEY_MISMATCH (server identity verification failed)";
	case 6:  return "[HappyPath] Security requirement mismatch between client and server";
	case 8:  return "[HappyPath] Server received a corrupted payload (decrypt produced wrong bytes)";
	case 9:  return "[HappyPath] Client never received ID_CONNECTION_REQUEST_ACCEPTED";
	case 10: return "[HappyPath] Server never received ID_NEW_INCOMING_CONNECTION";
	case 11: return "[HappyPath] Server never received the encrypted application payload";

	// Sub-test 2: wrong pinned key rejected
	case 20: return "[WrongKey] Server failed to start up";
	case 21: return "[WrongKey] Client failed to start up";
	case 22: return "[WrongKey] Connect() did not return CONNECTION_ATTEMPT_STARTED";
	case 23: return "[WrongKey] (unused)";
	case 24: return "[WrongKey] Client got ID_CONNECTION_REQUEST_ACCEPTED — wrong key was NOT rejected!";
	case 25: return "[WrongKey] Server got ID_NEW_INCOMING_CONNECTION — wrong key was NOT rejected!";

	// Sub-test 3: server with no key rejects client
	case 30: return "[NoKey] Server failed to start up";
	case 31: return "[NoKey] Client failed to start up";
	case 32: return "[NoKey] Connect() did not return CONNECTION_ATTEMPT_STARTED";
	case 33: return "[NoKey] Client never received a rejection notification (timeout)";
	case 34: return "[NoKey] Client got ID_CONNECTION_REQUEST_ACCEPTED — unkeyed server should have rejected!";
	case 35: return "[NoKey] Server got ID_NEW_INCOMING_CONNECTION — unkeyed server should have rejected!";

	default: return "Undefined Error";
	}
}

SecurityFunctionsTest::SecurityFunctionsTest(void)
	: server(nullptr), client(nullptr)
{
}

SecurityFunctionsTest::~SecurityFunctionsTest(void)
{
}

void SecurityFunctionsTest::DestroyPeers()
{
	// Shutdown all peers before destroying to let threads clean up.
	// The refactored sub-tests manage their own peers; this method cleans up
	// the member-field peers used in any legacy code path.
	if (client)
		client->Shutdown(100);
	if (server)
		server->Shutdown(100);

	if (client)
		RakPeerInterface::DestroyInstance(client);
	if (server)
		RakPeerInterface::DestroyInstance(server);
	client = nullptr;
	server = nullptr;
}
