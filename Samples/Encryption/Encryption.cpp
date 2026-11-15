/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 *
 *
 *  Modified work: Copyright (c) 2026, MafiaHub
 *
 *  Rewritten for encryption by default: there are no keys to generate, load or pass to Connect().
 */

// Every MafiaNet connection is encrypted by default: the connection handshake makes an ephemeral
// X25519 key exchange and every connected datagram is sealed with ChaCha20-Poly1305. This sample
// shows the three cases:
//   1. Two default peers connect encrypted, with no configuration at all.
//   2. Two peers that both opt out with EncryptionMode::Plaintext connect in clear (LAN debugging).
//   3. A Plaintext client is refused by a default server: there is no fallback to plaintext.

#include <cstdio>
#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"
#include "mafianet/types.h"

using namespace MafiaNet;

namespace
{
const char *ModeName(EncryptionMode mode)
{
	return mode == EncryptionMode::Required ? "Required" : "Plaintext";
}

const char *MessageName(MessageID id)
{
	switch (id)
	{
	case ID_CONNECTION_REQUEST_ACCEPTED:
		return "ID_CONNECTION_REQUEST_ACCEPTED";
	case ID_NEW_INCOMING_CONNECTION:
		return "ID_NEW_INCOMING_CONNECTION";
	case ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION:
		return "ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION (we are Plaintext, the server requires encryption)";
	case ID_OUR_SYSTEM_REQUIRES_SECURITY:
		return "ID_OUR_SYSTEM_REQUIRES_SECURITY (we require encryption, the server is Plaintext)";
	case ID_CONNECTION_ATTEMPT_FAILED:
		return "ID_CONNECTION_ATTEMPT_FAILED";
	case ID_PUBLIC_KEY_MISMATCH:
		return "ID_PUBLIC_KEY_MISMATCH (the server did not prove the pinned identity)";
	case ID_USER_PACKET_ENUM:
		return "ID_USER_PACKET_ENUM (our test message)";
	default:
		return "another message";
	}
}

// Starts a peer on an OS-assigned loopback port in the given mode (which must be set before Startup).
RakPeerInterface *StartPeer(EncryptionMode mode, unsigned short maxIncoming)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	peer->SetEncryptionMode(mode);
	SocketDescriptor sd(0, "127.0.0.1");
	if (peer->Startup(maxIncoming + 1, &sd, 1) != RAKNET_STARTED)
	{
		printf("Startup failed\n");
		RakPeerInterface::DestroyInstance(peer);
		return nullptr;
	}
	peer->SetMaximumIncomingConnections(maxIncoming);
	return peer;
}

// Connects client to server, prints what each side receives for up to two seconds, and sends one
// message once connected. Returns true if the client was accepted.
bool Run(const char *title, EncryptionMode serverMode, EncryptionMode clientMode)
{
	printf("\n%s\n", title);
	RakPeerInterface *server = StartPeer(serverMode, 4);
	RakPeerInterface *client = StartPeer(clientMode, 0);
	if (server == nullptr || client == nullptr)
	{
		if (server)
			RakPeerInterface::DestroyInstance(server);
		if (client)
			RakPeerInterface::DestroyInstance(client);
		return false;
	}
	printf("  server mode: %s, client mode: %s\n", ModeName(server->GetEncryptionMode()), ModeName(client->GetEncryptionMode()));

	const unsigned short port = server->GetMyBoundAddress().GetPort();
	client->Connect("127.0.0.1", port, nullptr, 0); // no PublicKey*: encrypted, server identity not pinned

	bool accepted = false;
	bool delivered = false;
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 2000 && !delivered)
	{
		for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
		{
			printf("  client got %s\n", MessageName(p->data[0]));
			if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
			{
				accepted = true;
				BitStream bs;
				bs.Write((MessageID) ID_USER_PACKET_ENUM);
				bs.Write((uint32_t) 42);
				client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, p->systemAddress, false);
			}
		}
		for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
		{
			printf("  server got %s\n", MessageName(p->data[0]));
			delivered = delivered || p->data[0] == ID_USER_PACKET_ENUM;
		}
		RakSleep(30);
	}

	server->Shutdown(100);
	client->Shutdown(100);
	RakPeerInterface::DestroyInstance(server);
	RakPeerInterface::DestroyInstance(client);
	return accepted;
}
} // namespace

int main(void)
{
	printf("MafiaNet connections are encrypted by default (EncryptionMode::Required).\n");
	RakPeerInterface *fresh = RakPeerInterface::GetInstance();
	printf("A new peer's GetEncryptionMode(): %s\n", ModeName(fresh->GetEncryptionMode()));
	RakPeerInterface::DestroyInstance(fresh);

	const bool encrypted = Run("1. Two default peers: encrypted, nothing to configure.", EncryptionMode::Required, EncryptionMode::Required);
	const bool plaintext = Run("2. Both peers opt out with SetEncryptionMode(EncryptionMode::Plaintext) before Startup().", EncryptionMode::Plaintext, EncryptionMode::Plaintext);
	const bool mismatch = Run("3. A Plaintext client and a default server: refused, no fallback.", EncryptionMode::Required, EncryptionMode::Plaintext);

	printf("\nencrypted connect: %s, plaintext connect: %s, mismatch refused: %s\n", encrypted ? "ok" : "FAILED", plaintext ? "ok" : "FAILED", !mismatch ? "ok" : "FAILED");
	return encrypted && plaintext && !mismatch ? 0 : 1;
}
