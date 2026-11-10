/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  What a connected remote may put in front of the application, over the
 *  in-process SimulatedNetwork (Tests/Support/SimulatedNetwork.h).
 */

#include <gtest/gtest.h>

#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer.h"

using namespace MafiaNet;

/*
The library tells the application about connections, sessions and authentication through messages it
generates itself: ID_NEW_INCOMING_CONNECTION, ID_CONNECTION_LOST, ID_SESSION_CONFIG_ABANDONED,
ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS and so on. An application acts on those as facts,
so a connected peer must not be able to make one appear.

RakPeer and the plugins refuse them from a remote by looking at the first byte. A message may start
with ID_TIMESTAMP and a time instead, with the real id after it, and every reader (Receive()'s
timestamp shift, the dispatcher, PeerHandle, applications) then treats that inner id as the message.
These tests send what a hostile connected peer would: a control id behind a timestamp, and the one
control id that was not refused even bare.
*/

namespace
{
const MessageID kSentinel = ID_USER_PACKET_ENUM + 3;
const unsigned int kInnerIdOffset = sizeof(MessageID) + sizeof(MafiaNet::Time);

void Send(RakPeer *from, const SystemAddress &to, const BitStream &bs)
{
	ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, to, false));
}

void SendBare(RakPeer *from, const SystemAddress &to, MessageID id)
{
	BitStream bs;
	bs.Write(id);
	bs.Write((unsigned char) 0x5a);
	Send(from, to, bs);
}

void SendTimestamped(RakPeer *from, const SystemAddress &to, MessageID innerId)
{
	BitStream bs;
	bs.Write((MessageID) ID_TIMESTAMP);
	bs.Write(MafiaNet::GetTime());
	bs.Write(innerId);
	bs.Write((unsigned char) 0x5a);
	Send(from, to, bs);
}

// Whether the application was handed a message with this id, bare or behind a timestamp.
bool Delivered(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
{
	for (const SimulatedNetwork::Message &message : net.Inbox(peer))
	{
		if (message.id == id)
			return true;
		if (message.id == ID_TIMESTAMP && message.data.size() > kInnerIdOffset && message.data[kInnerIdOffset] == id)
			return true;
	}
	return false;
}

struct ConnectedPair
{
	SimulatedNetwork net;
	RakPeer *server = nullptr;
	RakPeer *client = nullptr;

	void SetUp()
	{
		server = net.AddPeer(1, 1);
		client = net.AddPeer(1);
		ASSERT_TRUE(net.Connect(client, server));
		// What the connect itself produced is not what these tests are about.
		net.Inbox(server).clear();
	}

	// Everything sent before this has been through the server's receive path once it returns true:
	// the channel is reliable and ordered.
	bool FlushWithSentinel()
	{
		SendBare(client, net.AddressOf(server), kSentinel);
		return net.RunUntilReceived(server, kSentinel, 2000);
	}
};

class ForgedBehindTimestamp : public ::testing::TestWithParam<int>
{
};
} // namespace

TEST_P(ForgedBehindTimestamp, IsNotDeliveredAndTheConnectionCarriesOn)
{
	const MessageID forged = (MessageID) GetParam();
	ConnectedPair pair;
	ASSERT_NO_FATAL_FAILURE(pair.SetUp());

	SendTimestamped(pair.client, pair.net.AddressOf(pair.server), forged);
	ASSERT_TRUE(pair.FlushWithSentinel());

	EXPECT_FALSE(Delivered(pair.net, pair.server, forged)) << "id " << (int) forged << " reached the application behind a timestamp";
	EXPECT_EQ(IS_CONNECTED, pair.server->GetConnectionState(pair.net.AddressOf(pair.client)));
}

INSTANTIATE_TEST_SUITE_P(
    RemoteControlIds, ForgedBehindTimestamp,
    ::testing::Values(
        // Below ID_TIMESTAMP: refused bare, and each one tells the application about a connection.
        (int) ID_NEW_INCOMING_CONNECTION,
        (int) ID_CONNECTION_REQUEST_ACCEPTED,
        (int) ID_DISCONNECTION_NOTIFICATION,
        (int) ID_CONNECTION_LOST,
        (int) ID_CONNECTION_ATTEMPT_FAILED,
        // The session handshake: consumed by RakPeer bare, and only while the handshake runs.
        (int) ID_SESSION_CONFIG_REQUEST,
        (int) ID_SESSION_CONFIG,
        (int) ID_SESSION_CONFIG_STATUS,
        (int) ID_SESSION_CONFIG_REJECTED,
        (int) ID_SESSION_CONFIG_ABANDONED,
        // Plugin verdicts the plugins refuse from a remote by their first byte.
        (int) ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS,
        (int) ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE,
        (int) ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT));

TEST(RemoteControlIds, BareSessionAbandonedFromARemoteIsNotDelivered)
{
	// Server-only and generated locally: it tells the application a held session request went away.
	ConnectedPair pair;
	ASSERT_NO_FATAL_FAILURE(pair.SetUp());

	SendBare(pair.client, pair.net.AddressOf(pair.server), ID_SESSION_CONFIG_ABANDONED);
	ASSERT_TRUE(pair.FlushWithSentinel());

	EXPECT_FALSE(Delivered(pair.net, pair.server, ID_SESSION_CONFIG_ABANDONED));
	EXPECT_EQ(IS_CONNECTED, pair.server->GetConnectionState(pair.net.AddressOf(pair.client)));
}

TEST(RemoteControlIds, ATimestampWithNoMessageBehindItIsNotDelivered)
{
	// Readers index the id at a fixed offset past the time; nothing is there.
	ConnectedPair pair;
	ASSERT_NO_FATAL_FAILURE(pair.SetUp());

	BitStream bs;
	bs.Write((MessageID) ID_TIMESTAMP);
	bs.Write(MafiaNet::GetTime());
	Send(pair.client, pair.net.AddressOf(pair.server), bs);
	ASSERT_TRUE(pair.FlushWithSentinel());

	EXPECT_FALSE(pair.net.Received(pair.server, ID_TIMESTAMP));
	EXPECT_EQ(IS_CONNECTED, pair.server->GetConnectionState(pair.net.AddressOf(pair.client)));
}

TEST(RemoteControlIds, ATimestampedApplicationMessageIsDelivered)
{
	ConnectedPair pair;
	ASSERT_NO_FATAL_FAILURE(pair.SetUp());

	const MessageID userId = ID_USER_PACKET_ENUM + 9;
	SendTimestamped(pair.client, pair.net.AddressOf(pair.server), userId);
	ASSERT_TRUE(pair.FlushWithSentinel());

	EXPECT_TRUE(Delivered(pair.net, pair.server, userId));
}

TEST(RemoteControlIds, TheLibraryMessagesThatCarryATimestampAreDelivered)
{
	// ReplicaManager3 timestamps its serializes and the NAT punchthrough server its connect-at-time;
	// with no plugin attached to claim them they come out of Receive().
	for (const MessageID id : {(MessageID) ID_REPLICA_MANAGER_SERIALIZE, (MessageID) ID_NAT_CONNECT_AT_TIME})
	{
		ConnectedPair pair;
		ASSERT_NO_FATAL_FAILURE(pair.SetUp());

		SendTimestamped(pair.client, pair.net.AddressOf(pair.server), id);
		ASSERT_TRUE(pair.FlushWithSentinel());

		EXPECT_TRUE(Delivered(pair.net, pair.server, id)) << "id " << (int) id;
	}
}
