/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <vector>

#include "mafianet/peer.h"
#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/file_list.h"
#include "mafianet/file_list_transfer.h"
#include "mafianet/file_list_transfer_cb_interface.h"
#include "mafianet/bit_stream.h"
#include "mafianet/sleep.h"
#include "mafianet/get_time.h"

using namespace MafiaNet;

/*
Description:
An application may shut its peer down, or drop a transfer, from inside a callback that RakPeer::Receive
is dispatching: a plugin's OnReceive, a FileListTransfer download callback, or code handling a packet
Receive returned. Shutdown() runs every plugin's OnRakPeerShutdown and releases the packet pool while
those callers are still on the stack, so each of them must stop touching what the shutdown released.

Success conditions:
- A FileListTransfer receiver whose callback shuts the peer down, or removes the receiver, is
  dereferenced exactly once, whether the set completes in the header (empty set) or on its last file.
- A plugin that shuts the peer down in OnReceive stops the packet there: later plugins never see it,
  and Receive returns nothing for it.
- A packet Receive returned stays intact across Shutdown() and can still be deallocated after it, and
  the peer starts and exchanges packets again afterwards.

Failure conditions: a second OnDereference, a later plugin seeing the packet, a packet whose fields
change across Shutdown(), or a crash.
*/

namespace
{
	const int kConnectTimeoutMs = 15000;
	const int kWaitTimeoutMs    = 15000;

	struct PeerPair
	{
		RakPeerInterface *server = nullptr;
		RakPeerInterface *client = nullptr;
		SystemAddress serverAddress = UNASSIGNED_SYSTEM_ADDRESS;
		SystemAddress clientAddress = UNASSIGNED_SYSTEM_ADDRESS;

		PeerPair()
		{
			server = RakPeerInterface::GetInstance();
			client = RakPeerInterface::GetInstance();
		}

		~PeerPair()
		{
			client->Shutdown(0);
			server->Shutdown(0);
			RakPeerInterface::DestroyInstance(client);
			RakPeerInterface::DestroyInstance(server);
		}

		bool Connect(unsigned short port)
		{
			SocketDescriptor serverSocket(port, "127.0.0.1");
			SocketDescriptor clientSocket(0, "127.0.0.1");
			if (server->Startup(4, &serverSocket, 1) != RAKNET_STARTED || client->Startup(1, &clientSocket, 1) != RAKNET_STARTED)
				return false;
			server->SetMaximumIncomingConnections(4);
			if (client->Connect("127.0.0.1", port, 0, 0) != CONNECTION_ATTEMPT_STARTED)
				return false;

			const TimeMS entry = GetTimeMS();
			while ((clientAddress == UNASSIGNED_SYSTEM_ADDRESS || serverAddress == UNASSIGNED_SYSTEM_ADDRESS) && GetTimeMS() - entry < (TimeMS)kConnectTimeoutMs)
			{
				for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
					if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
						clientAddress = p->systemAddress;
				for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
					if (p->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
						serverAddress = p->systemAddress;
				RakSleep(5);
			}
			return clientAddress != UNASSIGNED_SYSTEM_ADDRESS && serverAddress != UNASSIGNED_SYSTEM_ADDRESS;
		}

		// Pumps both peers until done() holds or the wait runs out.
		template <typename Done>
		bool PumpUntil(Done done)
		{
			const TimeMS entry = GetTimeMS();
			while (!done() && GetTimeMS() - entry < (TimeMS)kWaitTimeoutMs)
			{
				for (Packet *p = server->Receive(); p; server->DeallocatePacket(p), p = server->Receive())
					;
				for (Packet *p = client->Receive(); p; client->DeallocatePacket(p), p = client->Receive())
					;
				RakSleep(1);
			}
			return done();
		}

		// Pumps both peers for a fixed time, for traffic that must not arrive.
		void Drain(int ms)
		{
			const TimeMS entry = GetTimeMS();
			PumpUntil([&] { return GetTimeMS() - entry >= (TimeMS)ms; });
		}
	};

	enum class Reaction
	{
		ShutdownPeer,
		RemoveReceiver,
	};

	// Reacts from the callback that ends the set, as a client refusing a join does.
	class ReactingReceiver : public FileListTransferCBInterface
	{
	public:
		RakPeerInterface *peer = nullptr;
		FileListTransfer *transfer = nullptr;
		SystemAddress sender = UNASSIGNED_SYSTEM_ADDRESS;
		Reaction reaction = Reaction::ShutdownPeer;
		int completions = 0;
		int dereferences = 0;

		bool OnFile(OnFileStruct *) override
		{
			return true;
		}

		void OnFileProgress(FileProgressStruct *) override {}

		bool OnDownloadComplete(DownloadCompleteStruct *) override
		{
			++completions;
			if (reaction == Reaction::ShutdownPeer)
				peer->Shutdown(0);
			else
				transfer->RemoveReceiver(sender);
			return false;
		}

		void OnDereference(void) override
		{
			++dereferences;
		}
	};

	void RunReactingTransfer(unsigned short port, bool withFile, Reaction reaction)
	{
		PeerPair peers;
		FileListTransfer serverTransfer;
		FileListTransfer clientTransfer;
		peers.server->AttachPlugin(&serverTransfer);
		peers.client->AttachPlugin(&clientTransfer);
		ASSERT_TRUE(peers.Connect(port));

		ReactingReceiver receiver;
		receiver.peer     = peers.client;
		receiver.transfer = &clientTransfer;
		receiver.sender   = peers.serverAddress;
		receiver.reaction = reaction;
		const unsigned short setId = clientTransfer.SetupReceive(&receiver, false, peers.serverAddress);

		FileList list;
		if (withFile)
		{
			const std::vector<char> bytes(900, 'x');
			list.AddFile("small.bin", "small.bin", bytes.data(), (unsigned)bytes.size(), (unsigned)bytes.size(), FileListNodeContext(0, 0, 0, 0));
		}
		serverTransfer.Send(&list, peers.server, peers.clientAddress, setId, Priority::Medium, 0);

		EXPECT_TRUE(peers.PumpUntil([&] { return receiver.completions > 0; }));
		// Anything still queued must not reach the dropped receiver either.
		peers.Drain(500);

		EXPECT_EQ(receiver.completions, 1);
		EXPECT_EQ(receiver.dereferences, 1);

		peers.server->DetachPlugin(&serverTransfer);
		peers.client->DetachPlugin(&clientTransfer);
	}

	const MessageID kProbe = ID_USER_PACKET_ENUM + 7;

	// Shuts the peer down on the probe, from inside Receive's plugin dispatch.
	class ShuttingPlugin : public PluginInterface2
	{
	public:
		int probes = 0;

		PluginReceiveResult OnReceive(Packet *packet) override
		{
			if (packet->data[0] != kProbe)
				return RR_CONTINUE_PROCESSING;
			++probes;
			GetRakPeerInterface()->Shutdown(0);
			return RR_CONTINUE_PROCESSING;
		}
	};

	// Attached after the shutting plugin; must never see the probe.
	class LaterPlugin : public PluginInterface2
	{
	public:
		int probes = 0;
		int shutdowns = 0;

		PluginReceiveResult OnReceive(Packet *packet) override
		{
			if (packet->data[0] == kProbe)
				++probes;
			return RR_CONTINUE_PROCESSING;
		}

		void OnRakPeerShutdown(void) override
		{
			++shutdowns;
		}
	};

	void SendProbe(PeerPair &peers)
	{
		BitStream out;
		out.Write(kProbe);
		out.Write((uint32_t)0x5AFEC0DE);
		peers.client->Send(&out, Priority::High, Reliability::ReliableOrdered, 0, peers.serverAddress, false);
	}
} // namespace

TEST(ShutdownDuringDispatch, EmptySetReceiverShutsThePeerDown)
{
	RunReactingTransfer(61231, false, Reaction::ShutdownPeer);
}

TEST(ShutdownDuringDispatch, LastFileReceiverShutsThePeerDown)
{
	RunReactingTransfer(61232, true, Reaction::ShutdownPeer);
}

TEST(ShutdownDuringDispatch, EmptySetReceiverRemovesItself)
{
	RunReactingTransfer(61233, false, Reaction::RemoveReceiver);
}

TEST(ShutdownDuringDispatch, LastFileReceiverRemovesItself)
{
	RunReactingTransfer(61234, true, Reaction::RemoveReceiver);
}

TEST(ShutdownDuringDispatch, PluginShutdownStopsThePacket)
{
	PeerPair peers;
	ShuttingPlugin shutting;
	LaterPlugin later;
	peers.server->AttachPlugin(&shutting);
	peers.server->AttachPlugin(&later);
	ASSERT_TRUE(peers.Connect(61235));

	SendProbe(peers);
	bool probeReturned = false;
	const TimeMS entry = GetTimeMS();
	while (shutting.probes == 0 && GetTimeMS() - entry < (TimeMS)kWaitTimeoutMs)
	{
		for (Packet *p = peers.server->Receive(); p; peers.server->DeallocatePacket(p), p = peers.server->Receive())
			probeReturned |= p->data[0] == kProbe;
		for (Packet *p = peers.client->Receive(); p; peers.client->DeallocatePacket(p), p = peers.client->Receive())
			;
		RakSleep(1);
	}

	EXPECT_EQ(shutting.probes, 1);
	EXPECT_EQ(later.probes, 0);
	EXPECT_EQ(later.shutdowns, 1);
	EXPECT_FALSE(probeReturned);
	EXPECT_FALSE(peers.server->IsActive());

	peers.server->DetachPlugin(&later);
	peers.server->DetachPlugin(&shutting);
}

TEST(ShutdownDuringDispatch, ReturnedPacketOutlivesShutdown)
{
	PeerPair peers;
	ASSERT_TRUE(peers.Connect(61236));

	SendProbe(peers);
	Packet *probe = nullptr;
	const TimeMS entry = GetTimeMS();
	while (probe == nullptr && GetTimeMS() - entry < (TimeMS)kWaitTimeoutMs)
	{
		for (Packet *p = peers.server->Receive(); p; p = peers.server->Receive())
		{
			if (p->data[0] == kProbe && probe == nullptr)
			{
				probe = p;
				continue;
			}
			peers.server->DeallocatePacket(p);
		}
		RakSleep(1);
	}
	ASSERT_NE(probe, nullptr);

	const unsigned int length = probe->length;
	const RakNetGUID guid     = probe->guid;
	const std::vector<unsigned char> data(probe->data, probe->data + probe->length);

	// As an application does when a packet it is handling makes it disconnect.
	peers.server->Shutdown(0);

	EXPECT_EQ(probe->length, length);
	EXPECT_EQ(probe->guid, guid);
	EXPECT_EQ(std::vector<unsigned char>(probe->data, probe->data + probe->length), data);
	peers.server->DeallocatePacket(probe);

	// The pool the packet went back to still serves a fresh session.
	peers.client->Shutdown(0);
	peers.serverAddress = peers.clientAddress = UNASSIGNED_SYSTEM_ADDRESS;
	ASSERT_TRUE(peers.Connect(61237));
	SendProbe(peers);
	bool received = false;
	const TimeMS again = GetTimeMS();
	while (!received && GetTimeMS() - again < (TimeMS)kWaitTimeoutMs)
	{
		for (Packet *p = peers.server->Receive(); p; peers.server->DeallocatePacket(p), p = peers.server->Receive())
			received |= p->data[0] == kProbe;
		for (Packet *p = peers.client->Receive(); p; peers.client->DeallocatePacket(p), p = peers.client->Receive())
			;
		RakSleep(1);
	}
	EXPECT_TRUE(received);
}
