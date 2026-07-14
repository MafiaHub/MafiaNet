/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "CommonFunctions.h"

using namespace MafiaNet;

namespace
{
	// How many times to retry a full connect -> close -> receive cycle before
	// declaring failure. Delivering ID_DISCONNECTION_NOTIFICATION to Receive()
	// depends on a reliable ACK round-trip completing, which in turn needs both
	// peers' internal network threads to be scheduled. On a CPU-starved CI runner
	// running the whole suite in one process, a single cycle can intermittently
	// miss its window even with a generous timeout. Re-driving a fresh exchange
	// recovers from that transient starvation. Wrong-payload failures are NOT
	// retried (see below), so this only absorbs transient misses, never real bugs.
	const int kMaxAttempts = 3;
	// Per-attempt wait for the notification. Comfortably above a healthy loopback
	// round-trip (milliseconds) even under load, and bounded so kMaxAttempts
	// attempts can't stall CI for long on a genuine failure.
	const int kReceiveTimeoutMs = 12000;

	// TryDisconnectCase outcomes. Non-negative values are concrete error codes
	// (translated to messages by DisconnectErrorToString below); negative values
	// are transient signals the caller may retry.
	const int kTransientNeverReceived = -1; // connected, but notification never surfaced
	const int kTransientConnectFailed = -2; // never established the connection

	SystemAddress LoopbackServerAddress(unsigned short serverPort)
	{
		SystemAddress serverAddr;
		serverAddr.SetBinaryAddress("127.0.0.1");
		serverAddr.SetPortHostOrder(serverPort);
		return serverAddr;
	}

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

	// Force both peers to drop any connection between them and drain their queues,
	// so a fresh connect can succeed. Used between cases and between retry attempts.
	// Closes silently (no notification) on both sides: after a transient miss the
	// server may still hold the connection (waiting for an ACK that never came),
	// which would make the client's reconnect race against an "already connected"
	// rejection. Explicitly tearing down both sides removes that race.
	void ResetConnection(RakPeerInterface *server, RakPeerInterface *client, const RakNetGUID &clientGuid, unsigned short serverPort)
	{
		SystemAddress serverAddr = LoopbackServerAddress(serverPort);
		client->CloseConnection(serverAddr, false);
		if (clientGuid != UNASSIGNED_RAKNET_GUID)
			server->CloseConnection(clientGuid, false);

		TimeMS entry = GetTimeMS();
		while (GetTimeMS() - entry < 10000)
		{
			Packet *p;
			for (p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
				;
			for (p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
				;
			if (!CommonFunctions::ConnectionStateMatchesOptions(client, serverAddr, true, true, true, true))
				break;
			RakSleep(30);
		}
	}

	// Run one connect -> CloseConnection(reason) -> receive-notification cycle and
	// verify the delivered notification.
	//
	// Returns:
	//   0                        success (notification delivered and verified)
	//   kTransientConnectFailed  never connected           — caller may retry
	//   kTransientNeverReceived  connected, no notification — caller may retry
	//   > 0                      hard verification failure (wrong payload) — do NOT retry
	//
	// On every non-success path the connection is reset, leaving a clean slate for
	// a retry or the next case.
	int TryDisconnectCase(RakPeerInterface *server, RakPeerInterface *client, unsigned short serverPort,
		const MafiaNet::BitStream *reasonData, bool expectPayload,
		unsigned char expectCode, const RakString &expectText,
		int noPayloadErr, int deserialErr, int codeErr, int textErr,
		bool isVerbose, const char *okMsg)
	{
		RakNetGUID clientGuid = ConnectAndGetClientGuid(server, client, serverPort);
		if (clientGuid == UNASSIGNED_RAKNET_GUID)
		{
			ResetConnection(server, client, clientGuid, serverPort);
			return kTransientConnectFailed;
		}

		server->CloseConnection(clientGuid, true, 0, MafiaNet::Priority::Low, reasonData);

		Packet *note = CommonFunctions::WaitAndReturnMessageWithID(client, ID_DISCONNECTION_NOTIFICATION, kReceiveTimeoutMs);
		if (note == 0)
		{
			// Transient: the reliable round-trip didn't surface the notification in
			// time. A fresh exchange (retry) typically recovers under load.
			ResetConnection(server, client, clientGuid, serverPort);
			return kTransientNeverReceived;
		}

		if (!expectPayload)
		{
			// Default / empty reason: the notification must be a bare 1-byte message.
			unsigned int len = note->length;
			client->DeallocatePacket(note);
			ResetConnection(server, client, clientGuid, serverPort);
			if (len != 1)
				return noPayloadErr; // hard failure: payload present when none was sent
			if (isVerbose)
				printf("%s\n", okMsg);
			return 0;
		}

		// Reason supplied: the notification must carry more than the bare 1-byte ID.
		if (note->length <= 1)
		{
			client->DeallocatePacket(note);
			ResetConnection(server, client, clientGuid, serverPort);
			return noPayloadErr;
		}

		// The body after the ID must round-trip the exact reason we sent.
		BitStream readBack(note->data + 1, note->length - 1, false);
		unsigned char gotCode = 0;
		RakString gotText;
		readBack.Read(gotCode);
		bool textOk = gotText.Deserialize(&readBack);
		client->DeallocatePacket(note);
		ResetConnection(server, client, clientGuid, serverPort);

		// Content mismatches are hard failures: a real regression in the reason
		// payload would fail these identically on every attempt, so they are never
		// retried away.
		if (!textOk)
			return deserialErr;
		if (gotCode != expectCode)
			return codeErr;
		if (gotText != expectText)
			return textErr;

		if (isVerbose)
			printf("%s code=%u text=\"%s\"\n", okMsg, (unsigned)gotCode, gotText.C_String());
		return 0;
	}

	// Run a case with bounded retries. Transient outcomes are retried up to
	// kMaxAttempts; a hard verification failure returns immediately. If every
	// attempt is transient, the dominant CI symptom (never received) is reported,
	// or the connect failure if we never managed to connect on the last attempt.
	int RunDisconnectCaseWithRetries(RakPeerInterface *server, RakPeerInterface *client, unsigned short serverPort,
		const MafiaNet::BitStream *reasonData, bool expectPayload,
		unsigned char expectCode, const RakString &expectText,
		int connectErr, int neverReceivedErr,
		int noPayloadErr, int deserialErr, int codeErr, int textErr,
		bool isVerbose, const char *okMsg)
	{
		int lastTransient = kTransientNeverReceived;
		for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
		{
			int result = TryDisconnectCase(server, client, serverPort, reasonData, expectPayload,
				expectCode, expectText, noPayloadErr, deserialErr, codeErr, textErr, isVerbose, okMsg);
			if (result >= 0)
				return result; // success (0) or hard failure (> 0)
			lastTransient = result;
			if (isVerbose && attempt + 1 < kMaxAttempts)
				printf("(transient miss, retrying %d/%d)\n", attempt + 2, kMaxAttempts);
		}
		return (lastTransient == kTransientConnectFailed) ? connectErr : neverReceivedErr;
	}

	// The legacy harness's ErrorCodeToString table, folded into assertion messages.
	const char *DisconnectErrorToString(int errorCode)
	{
		switch (errorCode)
		{
		case 0:  return "No error";
		case 3:  return "Case 1: client failed to connect / server saw no incoming connection";
		case 4:  return "Case 1: client never received ID_DISCONNECTION_NOTIFICATION";
		case 5:  return "Case 1: notification carried no reason payload (length <= 1)";
		case 6:  return "Case 1: reason string failed to deserialize";
		case 7:  return "Case 1: reason enum code did not round-trip";
		case 8:  return "Case 1: reason string did not round-trip";
		case 9:  return "Case 2: client failed to reconnect / server saw no incoming connection";
		case 10: return "Case 2: client never received ID_DISCONNECTION_NOTIFICATION";
		case 11: return "Case 2: reasonless notification was not a bare 1-byte message";
		case 12: return "Case 3: client failed to reconnect / server saw no incoming connection";
		case 13: return "Case 3: client never received ID_DISCONNECTION_NOTIFICATION";
		case 14: return "Case 3: empty-reason notification was not a bare 1-byte message";
		default: return "Undefined Error";
		}
	}
}

// Mirrors the legacy DestroyPeers(): both peers are shut down and destroyed in
// TearDown() so cleanup survives a mid-test ASSERT failure.
class DisconnectReason : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server = RakPeerInterface::GetInstance();
		client = RakPeerInterface::GetInstance();

		// Bind the server to an OS-assigned ephemeral port rather than a fixed one.
		// Most tests in this suite share port 60000; running them all in one process
		// (as CI does) means a prior test's socket can still be lingering on that port
		// when this one starts, disrupting the fresh connection and intermittently
		// swallowing the disconnect notification. An ephemeral port sidesteps that.
		SocketDescriptor sdServer(0, "127.0.0.1");
		ASSERT_EQ(server->Startup(8, &sdServer, 1), RAKNET_STARTED) << "Server failed to start";
		server->SetMaximumIncomingConnections(8);

		serverPort = server->GetInternalID().GetPort();

		SocketDescriptor sdClient(0, "127.0.0.1");
		ASSERT_EQ(client->Startup(1, &sdClient, 1), RAKNET_STARTED) << "Client failed to start";

		// Keep connections alive well past the default 10s timeout. The whole suite
		// runs in one process and CI runners are CPU-starved; if a peer's internal
		// thread is starved the connection can time out at ~10s and the reliable
		// disconnect notification (still being retransmitted from the resend buffer)
		// is dropped when the connection dies. A generous timeout lets the
		// notification get through under load; it costs nothing on a healthy run,
		// where it arrives in milliseconds.
		server->SetTimeoutTime(30000, UNASSIGNED_SYSTEM_ADDRESS);
		client->SetTimeoutTime(30000, UNASSIGNED_SYSTEM_ADDRESS);
	}

	void TearDown() override
	{
		server->Shutdown(100);
		client->Shutdown(100);
		RakPeerInterface::DestroyInstance(server);
		RakPeerInterface::DestroyInstance(client);
	}

	RakPeerInterface *server = nullptr;
	RakPeerInterface *client = nullptr;
	unsigned short serverPort = 0;
};

// The three cases share the server/client peers and run sequentially, exactly
// like the legacy test.
TEST_F(DisconnectReason, NotificationCarriesOptionalReasonPayload)
{
	// --- Case 1: graceful disconnect WITH a reason payload ---
	{
		// A representative reason: an enum code plus an admin-typed custom string,
		// exactly the "Kicked: <reason>" use case the feature targets.
		const unsigned char kReasonCode = 7;
		RakString kReasonText("Cheating detected in match #4821");

		BitStream reason;
		reason.Write(kReasonCode);
		kReasonText.Serialize(&reason);

		int err = RunDisconnectCaseWithRetries(server, client, serverPort, &reason, true,
			kReasonCode, kReasonText,
			/*connectErr*/ 3, /*neverReceivedErr*/ 4, /*noPayloadErr*/ 5,
			/*deserialErr*/ 6, /*codeErr*/ 7, /*textErr*/ 8,
			false, "Case 1 OK: reason delivered.");
		ASSERT_EQ(err, 0) << DisconnectErrorToString(err);
	}

	// --- Case 2: graceful disconnect WITHOUT a reason (nullptr default) ---
	{
		// Default reasonData == nullptr: notification must stay payload-less.
		int err = RunDisconnectCaseWithRetries(server, client, serverPort, nullptr, false,
			0, RakString(),
			/*connectErr*/ 9, /*neverReceivedErr*/ 10, /*noPayloadErr*/ 11,
			/*deserialErr*/ 11, /*codeErr*/ 11, /*textErr*/ 11,
			false, "Case 2 OK: reasonless notification is a bare 1-byte message.");
		ASSERT_EQ(err, 0) << DisconnectErrorToString(err);
	}

	// --- Case 3: graceful disconnect with an EMPTY (non-null) reason BitStream ---
	{
		// A non-null but empty BitStream exercises the GetNumberOfBytesUsed() > 0
		// guard (distinct from the nullptr check): no bytes were written, so the
		// notification must stay payload-less just like the nullptr case.
		BitStream emptyReason;
		int err = RunDisconnectCaseWithRetries(server, client, serverPort, &emptyReason, false,
			0, RakString(),
			/*connectErr*/ 12, /*neverReceivedErr*/ 13, /*noPayloadErr*/ 14,
			/*deserialErr*/ 14, /*codeErr*/ 14, /*textErr*/ 14,
			false, "Case 3 OK: empty reason BitStream yields a bare 1-byte message.");
		ASSERT_EQ(err, 0) << DisconnectErrorToString(err);
	}
}
