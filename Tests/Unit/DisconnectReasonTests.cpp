/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  CloseConnection(..., reasonData): the ID_DISCONNECTION_NOTIFICATION the
 *  remote side receives carries the reason bytes after the message ID, and is
 *  a bare 1-byte message when no reason (or an empty one) is given.
 *
 *  Runs over the in-process SimulatedNetwork, so the reliable notification
 *  always arrives and there is nothing to retry: a missing notification here is
 *  a real failure.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/string.h"

using namespace MafiaNet;

namespace
{
const uint32_t kNotificationBudgetMs = 2000;

// Connects a fresh client to the server, has the server close it with the
// given reason, and returns the notification the client received (or null).
const SimulatedNetwork::Message *KickWithReason(SimulatedNetwork &net, RakPeer *server, RakPeer *client,
                                                const BitStream *reasonData)
{
	if (!net.Connect(client, server))
	{
		ADD_FAILURE() << "client never connected";
		return nullptr;
	}
	const RakNetGUID clientGuid = net.Find(server, ID_NEW_INCOMING_CONNECTION)->guid;
	EXPECT_EQ(client->GetMyGUID(), clientGuid);

	server->CloseConnection(clientGuid, true, 0, Priority::Low, reasonData);
	if (!net.RunUntilReceived(client, ID_DISCONNECTION_NOTIFICATION, kNotificationBudgetMs))
	{
		ADD_FAILURE() << "client never received ID_DISCONNECTION_NOTIFICATION";
		return nullptr;
	}
	const SimulatedNetwork::Message *note = net.Find(client, ID_DISCONNECTION_NOTIFICATION);
	EXPECT_EQ(server->GetMyGUID(), note->guid);
	return note;
}
}

TEST(DisconnectReason, ReasonPayloadRoundTripsInTheNotification)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);

	// The "Kicked: <reason>" use case: an enum code plus an admin-typed string.
	const unsigned char kReasonCode = 7;
	const RakString kReasonText("Cheating detected in match #4821");
	BitStream reason;
	reason.Write(kReasonCode);
	kReasonText.Serialize(&reason);

	const SimulatedNetwork::Message *note = KickWithReason(net, server, client, &reason);
	ASSERT_NE(nullptr, note);
	ASSERT_GT(note->data.size(), 1u) << "notification carried no reason payload";
	EXPECT_EQ(1u + reason.GetNumberOfBytesUsed(), note->data.size());

	BitStream readBack(const_cast<unsigned char *>(note->data.data()) + 1, (unsigned int) note->data.size() - 1, false);
	unsigned char gotCode = 0;
	RakString gotText;
	ASSERT_TRUE(readBack.Read(gotCode));
	ASSERT_TRUE(gotText.Deserialize(&readBack)) << "reason string failed to deserialize";
	EXPECT_EQ(kReasonCode, gotCode);
	EXPECT_EQ(kReasonText, gotText);
}

TEST(DisconnectReason, NullReasonGivesABareOneByteNotification)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);

	const SimulatedNetwork::Message *note = KickWithReason(net, server, client, nullptr);
	ASSERT_NE(nullptr, note);
	EXPECT_EQ(1u, note->data.size());
}

// A non-null but empty BitStream exercises the GetNumberOfBytesUsed() > 0
// guard, distinct from the nullptr check.
TEST(DisconnectReason, EmptyReasonGivesABareOneByteNotification)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);

	BitStream emptyReason;
	const SimulatedNetwork::Message *note = KickWithReason(net, server, client, &emptyReason);
	ASSERT_NE(nullptr, note);
	EXPECT_EQ(1u, note->data.size());
}

// The legacy test ran the three cases back to back on the same pair of peers:
// each kick must free the slot so the same client can reconnect and be kicked
// again, and each notification must carry only its own reason.
TEST(DisconnectReason, SamePeersReconnectAndCarryEachReasonInTurn)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);
	const SystemAddress clientAddress = net.AddressOf(client);

	BitStream reason;
	reason.Write((unsigned char) 42);
	BitStream emptyReason;
	const BitStream *reasons[] = {&reason, nullptr, &emptyReason};
	const size_t expectedLength[] = {2u, 1u, 1u};

	for (int round = 0; round < 3; ++round)
	{
		SCOPED_TRACE(testing::Message() << "round " << round);
		net.Inbox(client).clear();
		net.Inbox(server).clear();

		const SimulatedNetwork::Message *note = KickWithReason(net, server, client, reasons[round]);
		ASSERT_NE(nullptr, note);
		ASSERT_EQ(expectedLength[round], note->data.size());
		if (round == 0)
		{
			EXPECT_EQ(42, note->data[1]);
		}

		// Both sides must release the slot before the next round's Connect().
		ASSERT_TRUE(net.RunUntil([&]
		                         {
			const ConnectionState c = client->GetConnectionState(serverAddress);
			const ConnectionState s = server->GetConnectionState(clientAddress);
			return (c == IS_NOT_CONNECTED || c == IS_DISCONNECTED) && (s == IS_NOT_CONNECTED || s == IS_DISCONNECTED); }, 5000));
	}
}
