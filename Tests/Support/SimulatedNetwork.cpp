/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "SimulatedNetwork.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

#include "mafianet/assert.h"
#include "mafianet/message_identifiers.h"

namespace MafiaNet
{
	namespace
	{
		// Starts away from zero: a time of 0 means "never" in several places.
		const TimeUS kEpochUS = 1000000;
		TimeUS g_virtualNowUS = kEpochUS;
		bool g_networkAlive = false;
	}

	// A bound endpoint on the simulated network. Send() hands the datagram to
	// the network; delivery happens in SimulatedNetwork::Step.
	class SimulatedNetwork::SimSocket : public RakNetSocket2
	{
	public:
		SimSocket(SimulatedNetwork *network, const SystemAddress &address)
			: network(network)
		{
			SetSocketType(RNS2T_USER);
			boundAddress = address;
		}

		RNS2SendResult Send(RNS2_SendParameters *sendParameters, const char *, unsigned int) override
		{
			network->Enqueue(boundAddress, sendParameters->systemAddress, sendParameters->data, sendParameters->length);
			return sendParameters->length;
		}

	private:
		SimulatedNetwork *network;
	};

	SimulatedNetwork::SimulatedNetwork(uint32_t seed)
		: lcgState(seed ? seed : 1)
	{
		RakAssert(!g_networkAlive && "only one SimulatedNetwork at a time");
		g_networkAlive = true;
		g_virtualNowUS = kEpochUS;
		SetTimeSourceOverride(&SimulatedNetwork::VirtualClock);
	}

	SimulatedNetwork::~SimulatedNetwork()
	{
		for (RakPeer *peer : peers)
		{
			peer->Shutdown(0);
			if (peersInStorage.count(peer) != 0)
				peer->~RakPeer();
			else
				RakPeerInterface::DestroyInstance(peer);
		}
		SetTimeSourceOverride(nullptr);
		g_networkAlive = false;
	}

	TimeUS SimulatedNetwork::VirtualClock()
	{
		return g_virtualNowUS;
	}

	TimeUS SimulatedNetwork::Now() const
	{
		return g_virtualNowUS;
	}

	RakPeer *SimulatedNetwork::AddPeer(unsigned int maxConnections, unsigned short maxIncoming, unsigned short port)
	{
		return StartPeer(static_cast<RakPeer *>(RakPeerInterface::GetInstance()), maxConnections, maxIncoming, port);
	}

	RakPeer *SimulatedNetwork::AddPeerInStorage(void *storage, unsigned int maxConnections, unsigned short maxIncoming,
		unsigned short port)
	{
		RakPeer *peer = new (storage) RakPeer();
		peersInStorage.insert(peer);
		return StartPeer(peer, maxConnections, maxIncoming, port);
	}

	RakPeer *SimulatedNetwork::StartPeer(RakPeer *peer, unsigned int maxConnections, unsigned short maxIncoming,
		unsigned short port)
	{
		peer->SetSocketFactory(this);
		peer->SetManualUpdate(true);

		char host[32];
		std::snprintf(host, sizeof(host), "10.0.0.%u", (unsigned) nextHost++);
		// Skip the OS address enumeration: the peer reports this address as
		// its own, so handshakes carry the same bytes on every machine.
		peer->SetInternalID(SystemAddress(host, port ? port : 60000), 0);

		SocketDescriptor descriptor(port ? port : 60000, host);
		const StartupResult result = peer->Startup(maxConnections, &descriptor, 1);
		RakAssert(result == RAKNET_STARTED);
		(void) result;
		peer->SetMaximumIncomingConnections(maxIncoming);

		peers.push_back(peer);
		inboxes[peer];
		return peer;
	}

	SystemAddress SimulatedNetwork::AddressOf(const RakPeer *peer) const
	{
		return const_cast<RakPeer *>(peer)->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS, 0);
	}

	RakNetSocket2 *SimulatedNetwork::Create(const SocketDescriptor &descriptor, unsigned int)
	{
		SystemAddress address(descriptor.hostAddress, descriptor.port);
		if (sockets.count(address))
			return nullptr; // port in use
		SimSocket *socket = new SimSocket(this, address);
		sockets[address] = socket;
		return socket;
	}

	void SimulatedNetwork::Destroy(RakNetSocket2 *socket)
	{
		for (auto it = sockets.begin(); it != sockets.end(); ++it)
		{
			if (it->second == socket)
			{
				sockets.erase(it);
				break;
			}
		}
		delete static_cast<SimSocket *>(socket);
	}

	void SimulatedNetwork::SetLink(const RakPeer *from, const RakPeer *to, const LinkRule &rule)
	{
		links[std::make_pair(AddressOf(from), AddressOf(to))] = rule;
	}

	void SimulatedNetwork::SetLinkBothWays(const RakPeer *a, const RakPeer *b, const LinkRule &rule)
	{
		SetLink(a, b, rule);
		SetLink(b, a, rule);
	}

	const SimulatedNetwork::LinkRule &SimulatedNetwork::RuleFor(const SystemAddress &from, const SystemAddress &to) const
	{
		auto it = links.find(std::make_pair(from, to));
		return it == links.end() ? defaultRule : it->second;
	}

	uint32_t SimulatedNetwork::NextRandom()
	{
		lcgState = lcgState * 1664525u + 1013904223u;
		return lcgState >> 8;
	}

	void SimulatedNetwork::InjectDatagram(const SystemAddress &from, const RakPeer *to, const unsigned char *data, int length)
	{
		InFlight datagram;
		datagram.deliverAt = g_virtualNowUS;
		datagram.sequence = nextSequence++;
		datagram.from = from;
		datagram.to = AddressOf(to);
		datagram.bytes.assign(data, data + length);
		inFlight.push_back(std::move(datagram));
	}

	void SimulatedNetwork::Enqueue(const SystemAddress &from, const SystemAddress &to, const char *data, int length)
	{
		++datagramsSent;
		if (tap)
			tap(from, to, data, length);
		const LinkRule &rule = RuleFor(from, to);
		if (rule.dark || length > rule.maxDatagramBytes ||
			(rule.lossPercent > 0 && (int) (NextRandom() % 100u) < rule.lossPercent))
		{
			++datagramsDropped;
			return;
		}
		uint32_t delayMs = rule.delayMs;
		if (rule.jitterMs)
			delayMs += NextRandom() % (rule.jitterMs + 1);

		InFlight datagram;
		datagram.deliverAt = g_virtualNowUS + (TimeUS) delayMs * 1000;
		datagram.sequence = nextSequence++;
		datagram.from = from;
		datagram.to = to;
		datagram.bytes.assign(data, data + length);
		inFlight.push_back(std::move(datagram));
	}

	void SimulatedNetwork::DeliverDue()
	{
		// Deliveries can enqueue replies; take the due set first.
		std::vector<InFlight> due;
		auto split = std::stable_partition(inFlight.begin(), inFlight.end(),
			[](const InFlight &d) { return d.deliverAt > g_virtualNowUS; });
		std::move(split, inFlight.end(), std::back_inserter(due));
		inFlight.erase(split, inFlight.end());
		std::sort(due.begin(), due.end(), [](const InFlight &a, const InFlight &b) {
			return a.deliverAt != b.deliverAt ? a.deliverAt < b.deliverAt : a.sequence < b.sequence;
		});

		for (InFlight &datagram : due)
		{
			auto it = sockets.find(datagram.to);
			if (it == sockets.end())
			{
				++datagramsDropped;
				continue;
			}
			SimSocket *socket = it->second;
			RNS2EventHandler *handler = socket->GetEventHandler();
			if (!handler)
				continue;
			if (datagram.bytes.empty() || datagram.bytes.size() > MAXIMUM_MTU_SIZE)
				continue;
			RNS2RecvStruct *recv = handler->AllocRNS2RecvStruct(_FILE_AND_LINE_);
			std::memcpy(recv->data, datagram.bytes.data(), datagram.bytes.size());
			recv->bytesRead = (int) datagram.bytes.size();
			recv->systemAddress = datagram.from;
			recv->timeRead = g_virtualNowUS;
			recv->socket = socket;
			handler->OnRNS2Recv(recv);
		}
	}

	void SimulatedNetwork::Step(uint32_t ms)
	{
		for (uint32_t tick = 0; tick < ms; ++tick)
		{
			g_virtualNowUS += 1000;
			DeliverDue();
			for (RakPeer *peer : peers)
			{
				peer->RunManualUpdate();
				if (receiveDisabled.count(peer) != 0)
					continue;
				for (Packet *packet = peer->Receive(); packet; packet = peer->Receive())
				{
					Message message;
					message.id = packet->length ? packet->data[0] : 0;
					message.data.assign(packet->data, packet->data + packet->length);
					message.systemAddress = packet->systemAddress;
					message.guid = packet->guid;
					inboxes[peer].push_back(std::move(message));
					peer->DeallocatePacket(packet);
				}
			}
		}
	}

	void SimulatedNetwork::SetReceiveEnabled(const RakPeer *peer, bool enabled)
	{
		if (enabled)
			receiveDisabled.erase(peer);
		else
			receiveDisabled.insert(peer);
	}

	bool SimulatedNetwork::RunUntil(const std::function<bool()> &pred, uint32_t maxSimMs, uint32_t stepMs)
	{
		for (uint32_t elapsed = 0; elapsed < maxSimMs; elapsed += stepMs)
		{
			if (pred())
				return true;
			Step(stepMs);
		}
		return pred();
	}

	std::vector<SimulatedNetwork::Message> &SimulatedNetwork::Inbox(const RakPeer *peer)
	{
		return inboxes[peer];
	}

	const SimulatedNetwork::Message *SimulatedNetwork::Find(const RakPeer *peer, MessageID id) const
	{
		auto it = inboxes.find(peer);
		if (it == inboxes.end())
			return nullptr;
		for (const Message &message : it->second)
			if (message.id == id)
				return &message;
		return nullptr;
	}

	bool SimulatedNetwork::RunUntilReceived(const RakPeer *peer, MessageID id, uint32_t maxSimMs)
	{
		return RunUntil([&] { return Received(peer, id); }, maxSimMs);
	}

	bool SimulatedNetwork::Connect(RakPeer *client, RakPeer *server, uint32_t maxSimMs)
	{
		const SystemAddress target = AddressOf(server);
		char host[64];
		target.ToString(false, host, sizeof(host));
		if (client->Connect(host, target.GetPort(), nullptr, 0) != CONNECTION_ATTEMPT_STARTED)
			return false;
		return RunUntil([&] {
			return Received(client, ID_CONNECTION_REQUEST_ACCEPTED) && Received(server, ID_NEW_INCOMING_CONNECTION);
		}, maxSimMs);
	}
}
