/*
 *  Copyright (c) 2024, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "DisconnectReasonTest.h"

#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "CommonFunctions.h"

using namespace MafiaNet;

namespace
{
	// Connect the client to the server over loopback and return the client's
	// guid as observed by the server (via ID_NEW_INCOMING_CONNECTION). Returns
	// UNASSIGNED_RAKNET_GUID on failure.
	RakNetGUID ConnectAndGetClientGuid(RakPeerInterface *server, RakPeerInterface *client, unsigned short serverPort)
	{
		if (!CommonFunctions::WaitAndConnect(client, (char *)"127.0.0.1", serverPort, 10000))
			return UNASSIGNED_RAKNET_GUID;

		Packet *inc = CommonFunctions::WaitAndReturnMessageWithID(server, ID_NEW_INCOMING_CONNECTION, 10000);
		if (inc == 0)
			return UNASSIGNED_RAKNET_GUID;

		RakNetGUID clientGuid = inc->guid;
		server->DeallocatePacket(inc);
		return clientGuid;
	}
}

int DisconnectReasonTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void)params;
	(void)noPauses;

	destroyList.Clear(false, _FILE_AND_LINE_);

	const unsigned short serverPort = 60000;

	RakPeerInterface *server = RakPeerInterface::GetInstance();
	RakPeerInterface *client = RakPeerInterface::GetInstance();
	destroyList.Push(server, _FILE_AND_LINE_);
	destroyList.Push(client, _FILE_AND_LINE_);

	SocketDescriptor sdServer(serverPort, "127.0.0.1");
	if (server->Startup(8, &sdServer, 1) != RAKNET_STARTED)
		return 1;
	server->SetMaximumIncomingConnections(8);

	SocketDescriptor sdClient(0, "127.0.0.1");
	if (client->Startup(1, &sdClient, 1) != RAKNET_STARTED)
		return 2;

	// --- Case 1: graceful disconnect WITH a reason payload ---
	{
		RakNetGUID clientGuid = ConnectAndGetClientGuid(server, client, serverPort);
		if (clientGuid == UNASSIGNED_RAKNET_GUID)
			return 3;

		// A representative reason: an enum code plus an admin-typed custom string,
		// exactly the "Kicked: <reason>" use case the feature targets.
		const unsigned char kReasonCode = 7;
		RakString kReasonText("Cheating detected in match #4821");

		BitStream reason;
		reason.Write(kReasonCode);
		kReasonText.Serialize(&reason);

		server->CloseConnection(clientGuid, true, 0, LOW_PRIORITY, &reason);

		Packet *note = CommonFunctions::WaitAndReturnMessageWithID(client, ID_DISCONNECTION_NOTIFICATION, 10000);
		if (note == 0)
			return 4;

		// The notification must carry more than the bare 1-byte ID.
		if (note->length <= 1)
		{
			client->DeallocatePacket(note);
			return 5;
		}

		// The body after the ID must round-trip the exact reason we sent.
		BitStream readBack(note->data + 1, note->length - 1, false);
		unsigned char gotCode = 0;
		RakString gotText;
		readBack.Read(gotCode);
		bool textOk = gotText.Deserialize(&readBack);
		client->DeallocatePacket(note);

		if (!textOk)
			return 6;
		if (gotCode != kReasonCode)
			return 7;
		if (gotText != kReasonText)
			return 8;

		if (isVerbose)
			printf("Case 1 OK: reason code=%u text=\"%s\" delivered.\n", (unsigned)gotCode, gotText.C_String());
	}

	// Make sure the client has fully torn down before reconnecting on the same port.
	{
		SystemAddress serverAddr;
		serverAddr.SetBinaryAddress("127.0.0.1");
		serverAddr.SetPortHostOrder(serverPort);
		TimeMS entry = GetTimeMS();
		while (CommonFunctions::ConnectionStateMatchesOptions(client, serverAddr, true, true, true, true) && GetTimeMS() - entry < 10000)
		{
			Packet *p;
			for (p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
				;
			for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
				;
			RakSleep(30);
		}
	}

	// --- Case 2: graceful disconnect WITHOUT a reason (nullptr default) ---
	{
		RakNetGUID clientGuid = ConnectAndGetClientGuid(server, client, serverPort);
		if (clientGuid == UNASSIGNED_RAKNET_GUID)
			return 9;

		// Default reasonData == nullptr: notification must stay payload-less.
		server->CloseConnection(clientGuid, true, 0, LOW_PRIORITY);

		Packet *note = CommonFunctions::WaitAndReturnMessageWithID(client, ID_DISCONNECTION_NOTIFICATION, 10000);
		if (note == 0)
			return 10;

		unsigned int len = note->length;
		client->DeallocatePacket(note);

		if (len != 1)
			return 11;

		if (isVerbose)
			printf("Case 2 OK: reasonless notification is a bare 1-byte message.\n");
	}

	return 0;
}

RakString DisconnectReasonTest::GetTestName()
{
	return "DisconnectReasonTest";
}

RakString DisconnectReasonTest::ErrorCodeToString(int errorCode)
{
	if (errorCode > 0 && (unsigned int)errorCode <= errorList.Size())
		return errorList[errorCode - 1];
	return "Undefined Error";
}

DisconnectReasonTest::DisconnectReasonTest(void)
{
	errorList.Push("Server failed to start", _FILE_AND_LINE_);
	errorList.Push("Client failed to start", _FILE_AND_LINE_);
	errorList.Push("Case 1: client failed to connect / server saw no incoming connection", _FILE_AND_LINE_);
	errorList.Push("Case 1: client never received ID_DISCONNECTION_NOTIFICATION", _FILE_AND_LINE_);
	errorList.Push("Case 1: notification carried no reason payload (length <= 1)", _FILE_AND_LINE_);
	errorList.Push("Case 1: reason string failed to deserialize", _FILE_AND_LINE_);
	errorList.Push("Case 1: reason enum code did not round-trip", _FILE_AND_LINE_);
	errorList.Push("Case 1: reason string did not round-trip", _FILE_AND_LINE_);
	errorList.Push("Case 2: client failed to reconnect / server saw no incoming connection", _FILE_AND_LINE_);
	errorList.Push("Case 2: client never received ID_DISCONNECTION_NOTIFICATION", _FILE_AND_LINE_);
	errorList.Push("Case 2: reasonless notification was not a bare 1-byte message", _FILE_AND_LINE_);
}

DisconnectReasonTest::~DisconnectReasonTest(void)
{
}

void DisconnectReasonTest::DestroyPeers()
{
	int theSize = destroyList.Size();
	for (int i = 0; i < theSize; i++)
		destroyList[i]->Shutdown(100);
	for (int i = 0; i < theSize; i++)
		RakPeerInterface::DestroyInstance(destroyList[i]);
}
