/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The MTU the connection handshake settles on, and the connect loop that
 *  probes for it, over the in-process SimulatedNetwork.
 *
 *  The handshake pads ID_OPEN_CONNECTION_REQUEST_1 down a ladder of sizes and
 *  keeps the largest one the path passed. That value is frozen for the life of
 *  the connection and sizes every datagram in BOTH directions, so:
 *  - both peers must end up on the same size, inside [MINIMUM, MAXIMUM];
 *  - splitting and reassembly must work at whatever that size is;
 *  - the accepting side must clamp an MTU a remote names on the wire, since
 *    ID_OPEN_CONNECTION_REQUEST_2 is unauthenticated.
 *
 *  A link rule's maxDatagramBytes is a path ceiling: datagrams above it vanish,
 *  as they would behind a router with a smaller MTU. The datagram the ladder
 *  sends for a rung of size M is M - UDP_HEADER_SIZE bytes.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_size.h"

using namespace MafiaNet;

namespace
{
	const MessageID ID_BIG = ID_USER_PACKET_ENUM + 1;

	size_t CountOf(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
	{
		size_t n = 0;
		for (const auto &message : net.Inbox(peer))
			n += message.id == id;
		return n;
	}

	// Mirrors the file-static OFFLINE_MESSAGE_DATA_ID in peer.cpp. Duplicated
	// deliberately: a test of what an unconforming peer can put on the wire
	// should pin the wire format itself.
	const unsigned char kOfflineMessageDataId[16] =
		{0x00,0xFF,0xFF,0x00,0xFE,0xFE,0xFE,0xFE,0xFD,0xFD,0xFD,0xFD,0x12,0x34,0x56,0x78};

	// The ID_OPEN_CONNECTION_REQUEST_2 an accepting peer parses, with a
	// caller-chosen MTU, written with the calls the real handshake uses.
	void WriteOpenConnectionRequest2(BitStream &out, const SystemAddress &bindingAddress, uint16_t mtu, const RakNetGUID &guid)
	{
		out.Write((MessageID) ID_OPEN_CONNECTION_REQUEST_2);
		out.WriteAlignedBytes(kOfflineMessageDataId, sizeof(kOfflineMessageDataId));
		out.Write(bindingAddress);
		out.Write(mtu);
		out.Write(guid);
	}

	// Injects a forged REQUEST_2 from an address no peer owns and returns the
	// MTU the server assigned that address, or -1 if it never assigned a slot.
	//
	// An address with no slot reports defaultMTUSize, which doubles as "not
	// yet", so callers must not forge an MTU equal to defaultMTUSize.
	int ForgeRequest2(SimulatedNetwork &net, RakPeer *server, uint16_t mtu, uint64_t guid)
	{
		SystemAddress forger("10.0.0.200", 4321);
		BitStream forged;
		WriteOpenConnectionRequest2(forged, net.AddressOf(server), mtu, RakNetGUID(guid));
		net.InjectDatagram(forger, server, forged.GetData(), (int) forged.GetNumberOfBytesUsed());

		const int unassigned = server->GetMTUSize(UNASSIGNED_SYSTEM_ADDRESS);
		if (!net.RunUntil([&] { return server->GetMTUSize(forger) != unassigned; }, 2000))
			return -1;
		return server->GetMTUSize(forger);
	}
}

// With no ceiling the top rung passes, so the value is exact -- which also
// proves the ladder does not silently start below MAXIMUM_MTU_SIZE.
TEST(MTUNegotiation, UnrestrictedPathNegotiatesTheMaximumOnBothSides)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));

	const int clientSide = client->GetMTUSize(net.AddressOf(server));
	const int serverSide = server->GetMTUSize(net.AddressOf(client));
	EXPECT_EQ(MAXIMUM_MTU_SIZE, clientSide);
	EXPECT_EQ(MAXIMUM_MTU_SIZE, serverSide);
	EXPECT_GE(clientSide, MINIMUM_MTU_SIZE);
}

// Behind a ceiling both peers must still agree, on a rung that fits it.
TEST(MTUNegotiation, PathCeilingGivesBothSidesTheSameRungBelowIt)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);
	SimulatedNetwork::LinkRule narrow;
	narrow.maxDatagramBytes = 1100;
	net.SetLinkBothWays(client, server, narrow);
	ASSERT_TRUE(net.Connect(client, server, 20000));

	const int clientSide = client->GetMTUSize(net.AddressOf(server));
	const int serverSide = server->GetMTUSize(net.AddressOf(client));
	EXPECT_EQ(clientSide, serverSide) << "the two peers disagree about the datagram size";
	EXPECT_LE(clientSide - UDP_HEADER_SIZE, 1100);
	EXPECT_GE(clientSide, MINIMUM_MTU_SIZE);
	EXPECT_LT(clientSide, MAXIMUM_MTU_SIZE);
}

// Splitting and reassembly happen at the negotiated MTU. Send a message many
// times that size and check it back byte for byte, at the maximum and behind a
// ceiling (where every fragment must also fit the path).
TEST(MTUNegotiation, MessageManyTimesTheMTUArrivesIntact)
{
	for (int ceiling : {0, 1100})
	{
		SCOPED_TRACE(testing::Message() << "ceiling " << ceiling);
		SimulatedNetwork net;
		RakPeer *server = net.AddPeer(8, 8);
		RakPeer *client = net.AddPeer(1);
		if (ceiling)
		{
			SimulatedNetwork::LinkRule narrow;
			narrow.maxDatagramBytes = ceiling;
			net.SetLinkBothWays(client, server, narrow);
		}
		ASSERT_TRUE(net.Connect(client, server, 20000));
		const uint64_t droppedAfterConnect = net.DatagramsDropped();

		const size_t payloadSize = (size_t) MAXIMUM_MTU_SIZE * 30;
		std::vector<char> payload(payloadSize);
		for (size_t i = 0; i < payloadSize; ++i)
			payload[i] = (char) ((i * 31 + (i >> 8)) & 0xFF);

		BitStream bs;
		bs.Write(ID_BIG);
		bs.Write(payload.data(), (unsigned int) payloadSize);
		ASSERT_NE(0u, client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, net.AddressOf(server), false));

		ASSERT_TRUE(net.RunUntilReceived(server, ID_BIG, 10000)) << "the split message never reassembled";
		const SimulatedNetwork::Message *received = net.Find(server, ID_BIG);
		ASSERT_EQ(payloadSize + 1, received->data.size());
		EXPECT_EQ(0, std::memcmp(received->data.data() + 1, payload.data(), payloadSize));
		EXPECT_EQ(droppedAfterConnect, net.DatagramsDropped()) << "a fragment was larger than the negotiated path";
	}
}

// Regression: the connect loop divided sendConnectionAttemptCount by the number
// of rungs in the ladder, so any count below the rung count divided by zero.
// With no ceiling the first attempt uses the top rung and passes, so the
// connection completes however small the budget is. Closing and reconnecting on
// the same pair also covers slot release between attempts.
TEST(MTUNegotiation, ConnectWorksWithFewerAttemptsThanMTURungs)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress serverAddress = net.AddressOf(server);
	char host[64];
	serverAddress.ToString(false, host, sizeof(host));

	for (unsigned attempts = 1; attempts <= 3; ++attempts)
	{
		SCOPED_TRACE(testing::Message() << "sendConnectionAttemptCount=" << attempts);
		const size_t accepted = CountOf(net, client, ID_CONNECTION_REQUEST_ACCEPTED);
		const size_t incoming = CountOf(net, server, ID_NEW_INCOMING_CONNECTION);
		const size_t closed = CountOf(net, server, ID_DISCONNECTION_NOTIFICATION);

		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED,
			client->Connect(host, serverAddress.GetPort(), nullptr, 0, nullptr, 0, attempts, 500, 0));
		ASSERT_TRUE(net.RunUntil([&] {
			return CountOf(net, client, ID_CONNECTION_REQUEST_ACCEPTED) > accepted &&
				CountOf(net, server, ID_NEW_INCOMING_CONNECTION) > incoming;
		}, 5000)) << "both peers did not report the connection";

		EXPECT_EQ(MAXIMUM_MTU_SIZE, client->GetMTUSize(serverAddress));

		client->CloseConnection(serverAddress, true);
		ASSERT_TRUE(net.RunUntil([&] { return CountOf(net, server, ID_DISCONNECTION_NOTIFICATION) > closed; }, 5000))
			<< "the server never saw the connection close";
		ASSERT_TRUE(net.RunUntil([&] {
			const ConnectionState state = client->GetConnectionState(serverAddress);
			return state == IS_NOT_CONNECTED || state == IS_DISCONNECTED;
		}, 5000)) << "the client never released its connection slot";
	}
}

// An unauthenticated REQUEST_2 can name any uint16. The value sizes datagrams
// built into MAXIMUM_MTU_SIZE buffers, so it must be clamped.
TEST(MTUNegotiation, ForgedOversizedMTUFromTheWireIsClamped)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);

	const int mtu = ForgeRequest2(net, server, 0xFFFF, 0x00C0FFEE00C0FFEEull);
	ASSERT_NE(-1, mtu) << "the server never accepted the forged handshake packet";
	EXPECT_EQ(MAXIMUM_MTU_SIZE, mtu) << "a peer-supplied MTU of 0xFFFF was not clamped";
}

// The other half of the clamp: an in-range size survives untouched. 1024 is a
// real rung and deliberately not defaultMTUSize (see ForgeRequest2).
TEST(MTUNegotiation, ForgedInRangeMTUIsKept)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(8, 8);
	ASSERT_NE(1024, server->GetMTUSize(UNASSIGNED_SYSTEM_ADDRESS));

	const int mtu = ForgeRequest2(net, server, 1024, 0x00DEFACED00DEFACull);
	ASSERT_NE(-1, mtu) << "the server never accepted the forged handshake packet";
	EXPECT_EQ(1024, mtu) << "an MTU within the limit was altered";
}
