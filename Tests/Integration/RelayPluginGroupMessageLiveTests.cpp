/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RelayPlugin::SendGroupMessage() takes a priority, reliability and ordering channel and
 *  sends them to the server, but the server read them and then relayed every group message
 *  as High / ReliableOrdered / channel 0. This checks, on the receiving client's reliability
 *  layer, that the relayed message carries what the sender asked for.
 */

#include <gtest/gtest.h>

#include "mafianet/relay_plugin.h"
#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/peer_handle.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"

#include <vector>

using namespace MafiaNet;

namespace
{
	// Records the reliability and ordering channel of every inbound RPE_GROUP_MSG_FROM_SERVER.
	class GroupMessageObserver : public PluginInterface2
	{
	public:
		struct Seen
		{
			Reliability reliability;
			unsigned char orderingChannel;
			unsigned char tag;
		};
		std::vector<Seen> seen;

		bool UsesReliabilityLayer(void) const override { return true; }
		void OnInternalPacket(InternalPacket *internalPacket, unsigned /*frameNumber*/, SystemAddress /*remoteSystemAddress*/, TimeMS /*time*/, int isSend) override
		{
			// Unsplit payload: ID_RELAY_PLUGIN, RPE_GROUP_MSG_FROM_SERVER, sender name, data.
			if (isSend || internalPacket->splitPacketCount > 0 || internalPacket->dataBitLength < 16)
				return;
			if (internalPacket->data[0] != ID_RELAY_PLUGIN || internalPacket->data[1] != RPE_GROUP_MSG_FROM_SERVER)
				return;
			const unsigned int bytes = BITS_TO_BYTES(internalPacket->dataBitLength);
			seen.push_back({internalPacket->reliability, internalPacket->orderingChannel, internalPacket->data[bytes - 1]});
		}
	};

	// Pump every peer; return true once pred() holds, false at the deadline.
	template <class Pred>
	bool PumpUntil(std::vector<Peer *> peers, Pred pred, std::vector<unsigned char> *clientRelayEvents = nullptr)
	{
		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 10000)
		{
			for (size_t i = 0; i < peers.size(); i++)
			{
				for (auto pkt : peers[i]->incoming())
				{
					if (clientRelayEvents && i > 0 && pkt->length >= 2 && pkt->data[0] == ID_RELAY_PLUGIN)
						clientRelayEvents->push_back(pkt->data[1]);
				}
			}
			if (pred())
				return true;
			RakSleep(30);
		}
		return pred();
	}
}

class RelayPluginGroupMessageLive : public ::testing::Test
{
	// Peers are RAII MafiaNet::Peer locals declared after their plugins, so they shut down
	// (and stop calling the plugins) before the plugins are destroyed.
};

TEST_F(RelayPluginGroupMessageLive, GroupMessageKeepsRequestedReliabilityAndChannel)
{
	RelayPlugin serverRelay, relayA, relayB;
	GroupMessageObserver observerB;

	// Space the constructions: POSIX GUIDs are seeded from the microsecond clock.
	Peer server;
	RakSleep(2);
	Peer clientA;
	RakSleep(2);
	Peer clientB;

	// Attach before Startup: the observer uses the reliability layer, whose plugin list the
	// network thread reads without a lock.
	server->AttachPlugin(&serverRelay);
	serverRelay.SetAcceptAddParticipantRequests(true);
	clientA->AttachPlugin(&relayA);
	clientB->AttachPlugin(&relayB);
	clientB->AttachPlugin(&observerB);

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(2, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(2);
	const unsigned short serverPort = server->GetInternalID().GetPort();

	SocketDescriptor clientSd(0, "127.0.0.1");
	ASSERT_EQ(clientA->Startup(1, &clientSd, 1), RAKNET_STARTED);
	ASSERT_EQ(clientB->Startup(1, &clientSd, 1), RAKNET_STARTED);

	ASSERT_EQ(clientA->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_EQ(clientB->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);

	std::vector<Peer *> peers{&server, &clientA, &clientB};
	ASSERT_TRUE(PumpUntil(peers, [&] {
		return server->NumberOfConnections() == 2 && clientA->NumberOfConnections() == 1 && clientB->NumberOfConnections() == 1;
	})) << "peers did not connect";

	const RakNetGUID serverGuid = server->GetMyGUID();
	std::vector<unsigned char> relayEvents;
	auto count = [&](unsigned char e) {
		size_t n = 0;
		for (unsigned char x : relayEvents)
			n += (x == e);
		return n;
	};

	relayA.AddParticipantRequestFromClient("a", serverGuid);
	relayB.AddParticipantRequestFromClient("b", serverGuid);
	ASSERT_TRUE(PumpUntil(peers, [&] { return count(RPE_ADD_CLIENT_SUCCESS) == 2; }, &relayEvents))
		<< "participants were not added";

	relayA.JoinGroupRequest(serverGuid, "room");
	relayB.JoinGroupRequest(serverGuid, "room");
	ASSERT_TRUE(PumpUntil(peers, [&] { return count(RPE_JOIN_GROUP_SUCCESS) == 2; }, &relayEvents))
		<< "clients did not join the group";

	// Two group messages from A, each with settings the old relay overwrote.
	BitStream ordered;
	ordered.Write((unsigned char) 1);
	relayA.SendGroupMessage(serverGuid, &ordered, Priority::Low, Reliability::ReliableOrdered, 7);
	BitStream reliable;
	reliable.Write((unsigned char) 2);
	relayA.SendGroupMessage(serverGuid, &reliable, Priority::Medium, Reliability::Reliable, 0);

	ASSERT_TRUE(PumpUntil(peers, [&] { return observerB.seen.size() >= 2; }, &relayEvents))
		<< "client B did not receive both group messages";

	bool sawOrdered = false, sawReliable = false;
	for (const auto &s : observerB.seen)
	{
		if (s.tag == 1)
		{
			sawOrdered = true;
			EXPECT_EQ(s.reliability, Reliability::ReliableOrdered);
			EXPECT_EQ(s.orderingChannel, 7);
		}
		else if (s.tag == 2)
		{
			sawReliable = true;
			EXPECT_EQ(s.reliability, Reliability::Reliable);
		}
	}
	EXPECT_TRUE(sawOrdered);
	EXPECT_TRUE(sawReliable);
}
