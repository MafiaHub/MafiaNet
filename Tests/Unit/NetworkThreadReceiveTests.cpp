/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  PluginInterface2::OnReceiveNetworkThread: a plugin that opts in sees every
 *  message from a connected system inside the update cycle, before it is queued
 *  for RakPeer::Receive(). These run on the SimulatedNetwork with the server's
 *  Receive() switched off, which is what a stalled application main thread looks
 *  like to the peer: the update cycle keeps running, Receive() is never called.
 */

#include <gtest/gtest.h>

#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;

namespace
{
	const MessageID ID_NT_CONSUMED = ID_USER_PACKET_ENUM + 20;
	const MessageID ID_NT_PASSED = ID_USER_PACKET_ENUM + 21;
	const MessageID ID_NT_KEPT = ID_USER_PACKET_ENUM + 22;
	const MessageID ID_NT_ECHO = ID_USER_PACKET_ENUM + 23;

	// Consumes ID_NT_CONSUMED, keeps ID_NT_KEPT (frees it later itself), echoes
	// ID_NT_ECHO back to its sender, passes everything else through.
	class NetworkThreadProbe : public PluginInterface2
	{
	public:
		explicit NetworkThreadProbe(bool optIn = true) : optIn(optIn) {}

		bool UsesNetworkThreadReceive(void) const override { return optIn; }

		PluginReceiveResult OnReceiveNetworkThread(Packet *packet) override
		{
			networkThreadIds.push_back(packet->data[0]);
			switch (packet->data[0])
			{
			case ID_NT_CONSUMED:
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			case ID_NT_KEPT:
				kept.push_back(packet);
				return RR_STOP_PROCESSING;
			case ID_NT_ECHO:
				GetRakPeerInterface()->Send((const char *) packet->data, (int) packet->length, Priority::High,
					Reliability::Reliable, 0, packet->guid, false);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			default:
				return RR_CONTINUE_PROCESSING;
			}
		}

		PluginReceiveResult OnReceive(Packet *packet) override
		{
			userThreadIds.push_back(packet->data[0]);
			return RR_CONTINUE_PROCESSING;
		}

		size_t NetworkThreadCount(MessageID id) const { return Count(networkThreadIds, id); }
		size_t UserThreadCount(MessageID id) const { return Count(userThreadIds, id); }

		std::vector<Packet *> kept;

	private:
		static size_t Count(const std::vector<MessageID> &ids, MessageID id)
		{
			size_t n = 0;
			for (MessageID seen : ids)
				n += seen == id ? 1 : 0;
			return n;
		}

		bool optIn;
		std::vector<MessageID> networkThreadIds;
		std::vector<MessageID> userThreadIds;
	};

	void SendId(RakPeer *from, const SystemAddress &to, MessageID id)
	{
		BitStream bs;
		bs.Write(id);
		bs.Write((uint32_t) 0xC0FFEEu);
		ASSERT_NE(0u, from->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, to, false));
	}

	size_t InboxCount(SimulatedNetwork &net, const RakPeer *peer, MessageID id)
	{
		size_t n = 0;
		for (const auto &message : net.Inbox(peer))
			n += message.id == id ? 1 : 0;
		return n;
	}

	struct Pair
	{
		SimulatedNetwork net;
		RakPeer *server = net.AddPeer(2, 2);
		RakPeer *client = net.AddPeer(1);

		bool Start() { return net.Connect(client, server); }
	};
}

TEST(NetworkThreadReceive, ConsumedMessageIsSeenWithoutReceiveAndNeverQueued)
{
	Pair p;
	NetworkThreadProbe probe;
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());

	p.net.SetReceiveEnabled(p.server, false);
	SendId(p.client, p.net.AddressOf(p.server), ID_NT_CONSUMED);
	ASSERT_TRUE(p.net.RunUntil([&] { return probe.NetworkThreadCount(ID_NT_CONSUMED) == 1; }, 2000));

	p.net.SetReceiveEnabled(p.server, true);
	p.net.Step(50);
	EXPECT_EQ(0u, InboxCount(p.net, p.server, ID_NT_CONSUMED));
	EXPECT_EQ(0u, probe.UserThreadCount(ID_NT_CONSUMED));
	p.server->DetachPlugin(&probe);
}

TEST(NetworkThreadReceive, ContinuedMessageStillReachesReceiveAndOnReceive)
{
	Pair p;
	NetworkThreadProbe probe;
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());

	p.net.SetReceiveEnabled(p.server, false);
	SendId(p.client, p.net.AddressOf(p.server), ID_NT_PASSED);
	ASSERT_TRUE(p.net.RunUntil([&] { return probe.NetworkThreadCount(ID_NT_PASSED) == 1; }, 2000));
	EXPECT_EQ(0u, InboxCount(p.net, p.server, ID_NT_PASSED));

	p.net.SetReceiveEnabled(p.server, true);
	ASSERT_TRUE(p.net.RunUntilReceived(p.server, ID_NT_PASSED, 2000));
	EXPECT_EQ(1u, InboxCount(p.net, p.server, ID_NT_PASSED));
	EXPECT_EQ(1u, probe.UserThreadCount(ID_NT_PASSED));
	EXPECT_EQ(1u, probe.NetworkThreadCount(ID_NT_PASSED));
	p.server->DetachPlugin(&probe);
}

TEST(NetworkThreadReceive, StopProcessingHandsOwnershipToThePlugin)
{
	Pair p;
	NetworkThreadProbe probe;
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());

	SendId(p.client, p.net.AddressOf(p.server), ID_NT_KEPT);
	ASSERT_TRUE(p.net.RunUntil([&] { return probe.kept.size() == 1; }, 2000));
	p.net.Step(50);
	EXPECT_EQ(0u, InboxCount(p.net, p.server, ID_NT_KEPT));

	// Still valid: the peer did not free it.
	Packet *kept = probe.kept.front();
	EXPECT_EQ(ID_NT_KEPT, kept->data[0]);
	EXPECT_EQ(p.client->GetMyGUID(), kept->guid);
	p.server->DeallocatePacket(kept);
	p.server->DetachPlugin(&probe);
}

TEST(NetworkThreadReceive, PluginCanReplyFromTheHookWhileReceiveIsStalled)
{
	Pair p;
	NetworkThreadProbe probe;
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());

	p.net.SetReceiveEnabled(p.server, false);
	SendId(p.client, p.net.AddressOf(p.server), ID_NT_ECHO);
	EXPECT_TRUE(p.net.RunUntilReceived(p.client, ID_NT_ECHO, 2000));
	p.server->DetachPlugin(&probe);
}

TEST(NetworkThreadReceive, PluginThatDoesNotOptInIsNeverCalledOnTheNetworkThread)
{
	Pair p;
	NetworkThreadProbe probe(false);
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());

	SendId(p.client, p.net.AddressOf(p.server), ID_NT_CONSUMED);
	ASSERT_TRUE(p.net.RunUntilReceived(p.server, ID_NT_CONSUMED, 2000));
	EXPECT_EQ(0u, probe.NetworkThreadCount(ID_NT_CONSUMED));
	EXPECT_EQ(1u, probe.UserThreadCount(ID_NT_CONSUMED));
	p.server->DetachPlugin(&probe);
}

TEST(NetworkThreadReceive, DetachedPluginIsNoLongerCalled)
{
	Pair p;
	NetworkThreadProbe probe;
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());
	p.server->DetachPlugin(&probe);

	SendId(p.client, p.net.AddressOf(p.server), ID_NT_CONSUMED);
	ASSERT_TRUE(p.net.RunUntilReceived(p.server, ID_NT_CONSUMED, 2000));
	EXPECT_EQ(0u, probe.NetworkThreadCount(ID_NT_CONSUMED));
}

TEST(NetworkThreadReceive, ConnectionEventsAreNotRoutedThroughTheHook)
{
	// Only application messages from connected systems go through the hook;
	// lifecycle notifications stay on Receive() where plugins already expect them.
	Pair p;
	NetworkThreadProbe probe;
	p.server->AttachPlugin(&probe);
	ASSERT_TRUE(p.Start());
	EXPECT_EQ(0u, probe.NetworkThreadCount(ID_NEW_INCOMING_CONNECTION));
	EXPECT_EQ(1u, probe.UserThreadCount(ID_NEW_INCOMING_CONNECTION));
	p.server->DetachPlugin(&probe);
}
