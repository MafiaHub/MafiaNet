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

End-to-end happy-path test of the Noise_NK encrypted connection handshake.

A server generates a ServerSecurityKey identity and installs it via
SetServerSecurityKey. A client pins the matching public key via the existing
PublicKey API (PKM_USE_KNOWN_PUBLIC_KEY) and connects. We assert:
  - the client receives ID_CONNECTION_REQUEST_ACCEPTED,
  - the server receives ID_NEW_INCOMING_CONNECTION,
  - a reliable-ordered application message sent client->server arrives intact
    (proving the keyed SecureSession encrypts/decrypts over the wire).

Success conditions:
  All steps complete within the timeout. Returns 0.

Failure conditions:
  Each distinct failure returns a unique nonzero code (see ErrorCodeToString).

Negative cases (wrong key, plaintext mismatch, etc.) are covered by Task 10.
*/

static const unsigned short SECURITY_TEST_PORT = 60010;

int SecurityFunctionsTest::RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses)
{
	server=RakPeerInterface::GetInstance();
	client=RakPeerInterface::GetInstance();

	SocketDescriptor serverSd(SECURITY_TEST_PORT, 0);
	if (server->Startup(8, &serverSd, 1)!=RAKNET_STARTED)
		return 1;
	server->SetMaximumIncomingConnections(8);

	// Generate the server's Noise_NK identity and enable encrypted incoming connections.
	ServerSecurityKey key = GenerateServerSecurityKey();
	server->SetServerSecurityKey(key);

	SocketDescriptor clientSd;
	if (client->Startup(1, &clientSd, 1)!=RAKNET_STARTED)
		return 2;

	// Pin the server's public key on the client side via the existing PublicKey API.
	PublicKey pk;
	pk.publicKeyMode = PKM_USE_KNOWN_PUBLIC_KEY;
	pk.remoteServerPublicKey = (char*) key.publicKey;
	pk.myPublicKey = 0;
	pk.myPrivateKey = 0;

	if (isVerbose)
		printf("Connecting client to encrypted server (Noise_NK)\n");

	if (client->Connect("127.0.0.1", SECURITY_TEST_PORT, 0, 0, &pk)!=CONNECTION_ATTEMPT_STARTED)
		return 3;

	bool clientGotAccepted=false;
	bool serverGotNewConnection=false;
	bool serverGotPayload=false;

	char payload[]="ENCRYPTED_PAYLOAD";
	payload[0]=(char)(ID_USER_PACKET_ENUM+1);
	bool payloadSent=false;

	TimeMS entryTime=GetTimeMS();
	while (GetTimeMS()-entryTime < 7000)
	{
		Packet *packet;

		for (packet=client->Receive(); packet; client->DeallocatePacket(packet), packet=client->Receive())
		{
			switch (packet->data[0])
			{
			case ID_CONNECTION_REQUEST_ACCEPTED:
				clientGotAccepted=true;
				if (isVerbose)
					printf("Client: ID_CONNECTION_REQUEST_ACCEPTED\n");
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				return 4;
			case ID_PUBLIC_KEY_MISMATCH:
				return 5;
			case ID_OUR_SYSTEM_REQUIRES_SECURITY:
			case ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY:
				return 6;
			default:
				break;
			}
		}

		// Once connected, send a reliable-ordered application message to the server.
		if (clientGotAccepted && payloadSent==false)
		{
			client->Send(payload, (int) strlen(payload)+1, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true);
			payloadSent=true;
		}

		for (packet=server->Receive(); packet; server->DeallocatePacket(packet), packet=server->Receive())
		{
			switch (packet->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
				serverGotNewConnection=true;
				if (isVerbose)
					printf("Server: ID_NEW_INCOMING_CONNECTION\n");
				break;
			default:
				if (packet->data[0]==(unsigned char)(ID_USER_PACKET_ENUM+1))
				{
					if (packet->length==(unsigned int)(strlen(payload)+1) &&
						memcmp(packet->data, payload, strlen(payload)+1)==0)
					{
						serverGotPayload=true;
						if (isVerbose)
							printf("Server: received intact encrypted payload\n");
					}
					else
					{
						return 8; // payload corrupted
					}
				}
				break;
			}
		}

		if (clientGotAccepted && serverGotNewConnection && serverGotPayload)
			break;

		RakSleep(30);
	}

	if (!clientGotAccepted)
		return 9;
	if (!serverGotNewConnection)
		return 10;
	if (!serverGotPayload)
		return 11;

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

	case 0:
		return "No error";

	case 1:
		return "Server failed to start up";

	case 2:
		return "Client failed to start up";

	case 3:
		return "Connect() did not return CONNECTION_ATTEMPT_STARTED";

	case 4:
		return "Client got ID_CONNECTION_ATTEMPT_FAILED";

	case 5:
		return "Client got ID_PUBLIC_KEY_MISMATCH (server identity verification failed)";

	case 6:
		return "Security requirement mismatch between client and server";

	case 8:
		return "Server received a corrupted payload (decrypt produced wrong bytes)";

	case 9:
		return "Client never received ID_CONNECTION_REQUEST_ACCEPTED";

	case 10:
		return "Server never received ID_NEW_INCOMING_CONNECTION";

	case 11:
		return "Server never received the encrypted application payload";

	default:
		return "Undefined Error";
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
	// Shutdown all peers before destroying to let threads clean up
	if (client)
		client->Shutdown(100);
	if (server)
		server->Shutdown(100);

	if (client)
		RakPeerInterface::DestroyInstance(client);
	if (server)
		RakPeerInterface::DestroyInstance(server);
}
