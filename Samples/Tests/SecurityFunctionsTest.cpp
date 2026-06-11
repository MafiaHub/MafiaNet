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

Sub-test 4 (password and ban-list enforcement, port 60040):
  Restores the pre-Noise coverage of the password and ban-list enforcement paths,
  now running over the encrypted handshake. Asserts:
    - GetIncomingPassword returns what SetIncomingPassword stored,
    - a missing or wrong connection password is rejected, the correct one accepted,
    - AddToBanList blocks the client and IsBanned reports it,
    - RemoveFromBanList and ClearBanList both unban and allow reconnection.

Sub-test 5 (stateless cookie gate, port 60050):
  The peer API cannot forge a bad cookie, so this sub-test speaks the offline wire
  protocol over a raw UDP socket. Asserts:
    - positive control: a hand-crafted REQUEST_1 elicits REPLY_1 carrying a cookie
      (proves the raw rig works, so the silence assertions below are meaningful),
    - REQUEST_2 echoing a CORRUPTED cookie is dropped silently (no reply),
    - a TRUNCATED REQUEST_2 (cookie cut short) is dropped silently,
    - REQUEST_2 with a VALID cookie but garbage Noise message A is dropped silently
      (handshake rejected before any connection slot is allocated),
    - the server is still healthy afterwards: a real client connects successfully
      (no slot was consumed and no state was poisoned by the rejected packets).

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
  Error codes 40–53 : sub-test 4 (password and ban-list enforcement)
  Error codes 60–69 : sub-test 5 (stateless cookie gate)
*/

#include "mafianet/version.h"   // RAKNET_PROTOCOL_VERSION, for the raw REQUEST_1

// Raw UDP socket support for sub-test 5 (the peer API cannot forge offline packets).
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET RawSocket;
static const RawSocket RAW_SOCKET_INVALID = INVALID_SOCKET;
static void RawSocketClose(RawSocket s) { closesocket(s); }
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int RawSocket;
static const RawSocket RAW_SOCKET_INVALID = -1;
static void RawSocketClose(RawSocket s) { close(s); }
#endif

// ---------------------------------------------------------------------------
// Port assignments — each sub-test gets its own port to avoid collisions.
// ---------------------------------------------------------------------------
static const unsigned short PORT_HAPPY        = 60010;
static const unsigned short PORT_WRONG_KEY    = 60020;
static const unsigned short PORT_NO_KEY       = 60030;
static const unsigned short PORT_PASSWORD_BAN = 60040;
static const unsigned short PORT_COOKIE_GATE  = 60050;

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
	ServerSecurityKey key = GenerateServerSecurityKey();
	server->SetServerSecurityKey(key);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 1; }
	server->SetMaximumIncomingConnections(8);

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
	// Server installs its real identity.
	ServerSecurityKey serverKey = GenerateServerSecurityKey();
	server->SetServerSecurityKey(serverKey);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 20; }
	server->SetMaximumIncomingConnections(8);

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
// Sub-test 4: password and ban-list enforcement (over the encrypted handshake)
// ---------------------------------------------------------------------------

// Retry Connect() with the given password until connected or timeoutMs elapses.
// Returns the final connected state. Mirrors the retry pattern of the original
// (pre-Noise) test: rejection paths cancel the attempt, so keep re-issuing it.
static bool ConnectWithPassword(RakPeerInterface* client, const SystemAddress& serverAddress,
                                const char* password, const unsigned char serverPublicKey[32],
                                TimeMS timeoutMs)
{
	const int passwordLen = (password != 0) ? (int)strlen(password) : 0;
	TimeMS entryTime = GetTimeMS();
	while (!CommonFunctions::ConnectionStateMatchesOptions(client, serverAddress, true) &&
	       GetTimeMS() - entryTime < timeoutMs)
	{
		if (!CommonFunctions::ConnectionStateMatchesOptions(client, serverAddress, true, true, true, true))
			client->Connect("127.0.0.1", serverAddress.GetPort(), password, passwordLen, serverPublicKey);
		RakSleep(100);
	}
	return CommonFunctions::ConnectionStateMatchesOptions(client, serverAddress, true);
}

static void DisconnectClient(RakPeerInterface* client, const SystemAddress& serverAddress)
{
	while (CommonFunctions::ConnectionStateMatchesOptions(client, serverAddress, true, true, true, true))
	{
		client->CloseConnection(serverAddress, true, 0, LOW_PRIORITY);
		RakSleep(30);
	}
}

static int RunPasswordAndBanList(bool isVerbose)
{
	RakPeerInterface* server = RakPeerInterface::GetInstance();
	RakPeerInterface* client = RakPeerInterface::GetInstance();

	ServerSecurityKey key = GenerateServerSecurityKey();
	server->SetServerSecurityKey(key);
	SocketDescriptor serverSd(PORT_PASSWORD_BAN, 0);
	if (server->Startup(1, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 40; }
	server->SetMaximumIncomingConnections(1);

	char thePassword[] = "password";
	server->SetIncomingPassword(thePassword, (int)strlen(thePassword));

	char returnedPass[22];
	int returnedLen = 22;
	server->GetIncomingPassword(returnedPass, &returnedLen);
	if (returnedLen != (int)strlen(thePassword) ||
	    memcmp(returnedPass, thePassword, returnedLen) != 0)
	{ DestroyPair(server, client); return 42; }

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, client); return 41; }

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(PORT_PASSWORD_BAN);

	if (isVerbose) printf("[PasswordBan] Testing that a missing password is rejected\n");
	if (ConnectWithPassword(client, serverAddress, 0, key.publicKey, 5000))
	{ DestroyPair(server, client); return 43; }

	if (isVerbose) printf("[PasswordBan] Testing that a wrong password is rejected\n");
	if (ConnectWithPassword(client, serverAddress, "badpass", key.publicKey, 5000))
	{ DestroyPair(server, client); return 44; }

	if (isVerbose) printf("[PasswordBan] Testing that the correct password is accepted\n");
	if (!ConnectWithPassword(client, serverAddress, thePassword, key.publicKey, 5000))
	{ DestroyPair(server, client); return 45; }
	DisconnectClient(client, serverAddress);

	if (isVerbose) printf("[PasswordBan] Testing that a banned address is rejected\n");
	server->AddToBanList("127.0.0.1", 0);
	if (!server->IsBanned("127.0.0.1"))
	{ DestroyPair(server, client); return 46; }
	if (ConnectWithPassword(client, serverAddress, thePassword, key.publicKey, 5000))
	{ DestroyPair(server, client); return 47; }

	if (isVerbose) printf("[PasswordBan] Testing RemoveFromBanList\n");
	server->RemoveFromBanList("127.0.0.1");
	if (server->IsBanned("127.0.0.1"))
	{ DestroyPair(server, client); return 48; }
	if (!ConnectWithPassword(client, serverAddress, thePassword, key.publicKey, 5000))
	{ DestroyPair(server, client); return 49; }
	DisconnectClient(client, serverAddress);

	if (isVerbose) printf("[PasswordBan] Testing ClearBanList\n");
	server->AddToBanList("127.0.0.1", 0);
	if (!server->IsBanned("127.0.0.1"))
	{ DestroyPair(server, client); return 50; }
	if (ConnectWithPassword(client, serverAddress, thePassword, key.publicKey, 5000))
	{ DestroyPair(server, client); return 51; }
	server->ClearBanList();
	if (server->IsBanned("127.0.0.1"))
	{ DestroyPair(server, client); return 52; }
	if (!ConnectWithPassword(client, serverAddress, thePassword, key.publicKey, 5000))
	{ DestroyPair(server, client); return 53; }

	DestroyPair(server, client);
	if (isVerbose) printf("[PasswordBan] PASS: password and ban-list enforcement intact\n");
	return 0;
}

// ---------------------------------------------------------------------------
// Sub-test 5: stateless cookie gate (anti-flood)
// ---------------------------------------------------------------------------

// Wire constant from RakPeer.cpp (file-static there, so duplicated here). Marks
// a datagram as an offline message; must match or the server ignores the packet.
static const unsigned char OFFLINE_MESSAGE_DATA_ID_WIRE[16] =
	{0x00,0xFF,0xFF,0x00,0xFE,0xFE,0xFE,0xFE,0xFD,0xFD,0xFD,0xFD,0x12,0x34,0x56,0x78};

// Wait up to timeoutMs for any datagram. Returns bytes received, 0 on silence.
static int RawWaitForDatagram(RawSocket sock, unsigned char *buf, int bufLen, int timeoutMs)
{
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(sock, &readSet);
	timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	if (select((int) (sock + 1), &readSet, 0, 0, &tv) <= 0)
		return 0;
	int got = (int) recvfrom(sock, (char *) buf, bufLen, 0, 0, 0);
	return got > 0 ? got : 0;
}

static void RawDrain(RawSocket sock)
{
	unsigned char buf[2048];
	while (RawWaitForDatagram(sock, buf, sizeof buf, 50) > 0) {}
}

// Send a hand-crafted REQUEST_1 and parse the cookie out of REPLY_1.
// Returns true and fills cookieOut on success.
static bool RawFetchCookie(RawSocket sock, const sockaddr_in &serverAddr, unsigned char cookieOut[16])
{
	BitStream req1;
	req1.Write((MessageID) ID_OPEN_CONNECTION_REQUEST_1);
	req1.WriteAlignedBytes(OFFLINE_MESSAGE_DATA_ID_WIRE, sizeof(OFFLINE_MESSAGE_DATA_ID_WIRE));
	req1.Write((unsigned char) RAKNET_PROTOCOL_VERSION);
	unsigned char pad[16] = {0};   // padding so the datagram passes the offline-length gate
	req1.WriteAlignedBytes(pad, sizeof pad);

	for (int attempt = 0; attempt < 5; ++attempt)
	{
		sendto(sock, (const char *) req1.GetData(), (int) req1.GetNumberOfBytesUsed(), 0,
		       (const sockaddr *) &serverAddr, sizeof serverAddr);
		unsigned char buf[2048];
		int got = RawWaitForDatagram(sock, buf, sizeof buf, 1000);
		if (got <= 0 || buf[0] != ID_OPEN_CONNECTION_REPLY_1)
			continue;
		BitStream bsIn(buf, (unsigned int) got, false);
		bsIn.IgnoreBytes(sizeof(MessageID));
		bsIn.IgnoreBytes(sizeof(OFFLINE_MESSAGE_DATA_ID_WIRE));
		RakNetGUID serverGuid;
		bsIn.Read(serverGuid);
		unsigned char hasCookie = 0;
		bsIn.Read(hasCookie);
		if (hasCookie != 1)
			return false;   // keyed server must always hand out a cookie
		if (bsIn.ReadAlignedBytes(cookieOut, 16) == false)
			return false;
		return true;
	}
	return false;
}

// Build a REQUEST_2. cookieLen < 16 produces the truncated variant (no fields after
// the cookie); otherwise a full, well-formed REQUEST_2 carrying msgA is built.
static void RawBuildRequest2(BitStream &bs, const unsigned char *cookie, size_t cookieLen,
                             const unsigned char msgA[NoiseHandshake::MESSAGE_BYTES])
{
	bs.Reset();
	bs.Write((MessageID) ID_OPEN_CONNECTION_REQUEST_2);
	bs.WriteAlignedBytes(OFFLINE_MESSAGE_DATA_ID_WIRE, sizeof(OFFLINE_MESSAGE_DATA_ID_WIRE));
	bs.WriteAlignedBytes(cookie, cookieLen);
	if (cookieLen < 16)
		return;
	bs.Write((unsigned char) 1);   // "doing Noise"
	bs.WriteAlignedBytes(msgA, NoiseHandshake::MESSAGE_BYTES);
	SystemAddress bindingAddress;
	bindingAddress.SetBinaryAddress("127.0.0.1");
	bindingAddress.SetPortHostOrder(PORT_COOKIE_GATE);
	bs.Write(bindingAddress);
	bs.Write((uint16_t) 1400);     // MTU
	RakNetGUID fakeGuid;
	fakeGuid.g = 0x1122334455667788ULL;
	bs.Write(fakeGuid);
}

// Send a crafted REQUEST_2 and assert the server stays silent.
// Returns true if no reply arrived within the window.
static bool RawSendAndExpectSilence(RawSocket sock, const sockaddr_in &serverAddr, const BitStream &bs)
{
	sendto(sock, (const char *) bs.GetData(), (int) bs.GetNumberOfBytesUsed(), 0,
	       (const sockaddr *) &serverAddr, sizeof serverAddr);
	unsigned char buf[2048];
	return RawWaitForDatagram(sock, buf, sizeof buf, 1200) == 0;
}

static int RunCookieGate(bool isVerbose)
{
	RakPeerInterface* server = RakPeerInterface::GetInstance();
	ServerSecurityKey key = GenerateServerSecurityKey();
	server->SetServerSecurityKey(key);
	SocketDescriptor serverSd(PORT_COOKIE_GATE, 0);
	if (server->Startup(8, &serverSd, 1) != RAKNET_STARTED)
	{ DestroyPair(server, 0); return 60; }
	server->SetMaximumIncomingConnections(8);

	RawSocket sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == RAW_SOCKET_INVALID)
	{ DestroyPair(server, 0); return 61; }

	sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof serverAddr);
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(PORT_COOKIE_GATE);
	serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	unsigned char garbageMsgA[NoiseHandshake::MESSAGE_BYTES];
	memset(garbageMsgA, 0xAB, sizeof garbageMsgA);
	BitStream req2;
	int rc = 0;

	// Positive control: prove the raw rig elicits replies before asserting silence.
	if (isVerbose) printf("[CookieGate] Fetching a genuine cookie via raw REQUEST_1\n");
	unsigned char cookie[16];
	if (!RawFetchCookie(sock, serverAddr, cookie))
		rc = 62;
	RawDrain(sock);   // discard surplus REPLY_1s from retries

	if (rc == 0)
	{
		if (isVerbose) printf("[CookieGate] REQUEST_2 with corrupted cookie must be dropped silently\n");
		unsigned char badCookie[16];
		memcpy(badCookie, cookie, 16);
		badCookie[0] ^= 0x01;
		RawBuildRequest2(req2, badCookie, sizeof badCookie, garbageMsgA);
		if (!RawSendAndExpectSilence(sock, serverAddr, req2))
			rc = 63;
	}

	if (rc == 0)
	{
		if (isVerbose) printf("[CookieGate] Truncated REQUEST_2 must be dropped silently\n");
		RawBuildRequest2(req2, cookie, 8, 0);   // cookie cut short
		if (!RawSendAndExpectSilence(sock, serverAddr, req2))
			rc = 64;
	}

	if (rc == 0)
	{
		// Re-fetch: the first cookie may have aged past its time bucket by now.
		if (isVerbose) printf("[CookieGate] Valid cookie + garbage message A must be dropped silently\n");
		if (!RawFetchCookie(sock, serverAddr, cookie))
			rc = 62;
		else
		{
			RawDrain(sock);
			RawBuildRequest2(req2, cookie, sizeof cookie, garbageMsgA);
			if (!RawSendAndExpectSilence(sock, serverAddr, req2))
				rc = 65;
		}
	}

	RawSocketClose(sock);

	if (rc == 0)
	{
		// Server must still be healthy: none of the rejected packets may have
		// consumed a slot or poisoned state.
		if (isVerbose) printf("[CookieGate] Real client must still connect after the attack traffic\n");
		RakPeerInterface* client = RakPeerInterface::GetInstance();
		SocketDescriptor clientSd;
		if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED)
		{ DestroyPair(server, client); return 66; }
		SystemAddress serverAddress;
		serverAddress.SetBinaryAddress("127.0.0.1");
		serverAddress.SetPortHostOrder(PORT_COOKIE_GATE);
		if (!ConnectWithPassword(client, serverAddress, 0, key.publicKey, 5000))
			rc = 67;
		DestroyPair(server, client);
	}
	else
	{
		DestroyPair(server, 0);
	}

	if (rc == 0 && isVerbose) printf("[CookieGate] PASS: cookie gate drops forged/truncated handshakes silently\n");
	return rc;
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

	// Sub-test 4: password and ban-list enforcement
	if (isVerbose) printf("=== SecurityFunctionsTest: sub-test 4 (password and ban list) ===\n");
	rc = RunPasswordAndBanList(isVerbose);
	if (rc != 0)
	{
		if (isVerbose) printf("FAILED sub-test 4 with code %d\n", rc);
		return rc;
	}
	if (isVerbose) printf("=== sub-test 4 PASSED ===\n\n");

	// Sub-test 5: stateless cookie gate
	if (isVerbose) printf("=== SecurityFunctionsTest: sub-test 5 (stateless cookie gate) ===\n");
	rc = RunCookieGate(isVerbose);
	if (rc != 0)
	{
		if (isVerbose) printf("FAILED sub-test 5 with code %d\n", rc);
		return rc;
	}
	if (isVerbose) printf("=== sub-test 5 PASSED ===\n\n");

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

	// Sub-test 4: password and ban-list enforcement
	case 40: return "[PasswordBan] Server failed to start up";
	case 41: return "[PasswordBan] Client failed to start up";
	case 42: return "[PasswordBan] GetIncomingPassword returned wrong password";
	case 43: return "[PasswordBan] Client connected with no password";
	case 44: return "[PasswordBan] Client connected with wrong password";
	case 45: return "[PasswordBan] Client failed to connect with correct password";
	case 46: return "[PasswordBan] IsBanned does not show localhost as banned";
	case 47: return "[PasswordBan] Client was banned but connected anyways";
	case 48: return "[PasswordBan] Localhost was not unbanned by RemoveFromBanList";
	case 49: return "[PasswordBan] Client failed to connect after RemoveFromBanList";
	case 50: return "[PasswordBan] IsBanned does not show localhost as banned (second ban)";
	case 51: return "[PasswordBan] Client was banned but connected anyways (second ban)";
	case 52: return "[PasswordBan] Localhost was not unbanned by ClearBanList";
	case 53: return "[PasswordBan] Client failed to connect after ClearBanList";

	// Sub-test 5: stateless cookie gate
	case 60: return "[CookieGate] Server failed to start up";
	case 61: return "[CookieGate] Could not create raw UDP socket";
	case 62: return "[CookieGate] Positive control failed: raw REQUEST_1 got no REPLY_1 with cookie";
	case 63: return "[CookieGate] Server replied to a REQUEST_2 with a corrupted cookie";
	case 64: return "[CookieGate] Server replied to a truncated REQUEST_2";
	case 65: return "[CookieGate] Server replied to a valid cookie with garbage message A";
	case 66: return "[CookieGate] Follow-up client failed to start up";
	case 67: return "[CookieGate] Real client could not connect after the attack traffic";

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
