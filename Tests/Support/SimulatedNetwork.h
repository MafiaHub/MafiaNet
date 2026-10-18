/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  An in-process network for hermetic RakPeer tests: no sockets, no threads,
 *  no wall clock.
 *
 *  Peers created here get their sockets from this network (RakPeer's socket
 *  factory seam), run without internal threads (RakPeer::SetManualUpdate), and
 *  read a virtual clock this network installs. Step() advances that clock,
 *  delivers the datagrams that are due, runs every peer's update cycle and
 *  collects what each peer's Receive() returns. A whole connect / transfer /
 *  timeout scenario therefore runs in milliseconds and gives the same result on
 *  every run, so it belongs in the unit suite.
 *
 *  Per-direction link rules model loss, latency, jitter (and so reordering), a
 *  path MTU, or a link that has gone dark. Randomness comes from a seeded LCG.
 *
 *  Only one SimulatedNetwork may exist at a time: the clock override is process
 *  wide. Do not run a real (threaded, loopback) peer while one exists.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "mafianet/get_time.h"
#include "mafianet/peer.h"
#include "mafianet/socket2.h"
#include "mafianet/types.h"

namespace MafiaNet
{
	class SimulatedNetwork : public RakNetSocket2Factory
	{
	public:
		struct LinkRule
		{
			int lossPercent = 0;
			uint32_t delayMs = 1;
			uint32_t jitterMs = 0;
			int maxDatagramBytes = MAXIMUM_MTU_SIZE; // larger datagrams are dropped
			bool dark = false;                       // drop everything
		};

		// A message a peer's Receive() returned, copied so it outlives the Packet.
		struct Message
		{
			MessageID id;
			std::vector<unsigned char> data;
			SystemAddress systemAddress;
			RakNetGUID guid;
		};

		explicit SimulatedNetwork(uint32_t seed = 1);
		~SimulatedNetwork() override;

		SimulatedNetwork(const SimulatedNetwork &) = delete;
		SimulatedNetwork &operator=(const SimulatedNetwork &) = delete;

		// A started peer bound to 10.0.0.<n>:<port> (port 0 picks one).
		RakPeer *AddPeer(unsigned int maxConnections, unsigned short maxIncoming = 0, unsigned short port = 0);
		// The same, but the peer is constructed in caller-owned storage (at least
		// sizeof(RakPeer) bytes, suitably aligned) that must outlive this network.
		// Lets a test pre-fill that memory to prove the constructor sets what it must.
		RakPeer *AddPeerInStorage(void *storage, unsigned int maxConnections, unsigned short maxIncoming = 0,
			unsigned short port = 0);
		SystemAddress AddressOf(const RakPeer *peer) const;

		// Rules apply per direction; unset links use the default rule.
		void SetDefaultLink(const LinkRule &rule) { defaultRule = rule; }
		void SetLink(const RakPeer *from, const RakPeer *to, const LinkRule &rule);
		void SetLinkBothWays(const RakPeer *a, const RakPeer *b, const LinkRule &rule);

		// Advance the clock by ms in 1ms ticks: deliver due datagrams, run every
		// peer's update, collect received messages.
		void Step(uint32_t ms = 10);

		// Step until pred() holds or maxSimMs of simulated time passed.
		bool RunUntil(const std::function<bool()> &pred, uint32_t maxSimMs, uint32_t stepMs = 10);

		// While disabled, Step() still runs the peer's update cycle (its "network
		// thread") but never calls its Receive(): models an application whose main
		// thread has stalled. Packets queue inside the peer until re-enabled.
		void SetReceiveEnabled(const RakPeer *peer, bool enabled);

		std::vector<Message> &Inbox(const RakPeer *peer);
		// First message with this id in peer's inbox, or null.
		const Message *Find(const RakPeer *peer, MessageID id) const;
		bool Received(const RakPeer *peer, MessageID id) const { return Find(peer, id) != nullptr; }
		// Wait until peer has received a message with this id.
		bool RunUntilReceived(const RakPeer *peer, MessageID id, uint32_t maxSimMs);
		// Client connects to server and both see the connection.
		bool Connect(RakPeer *client, RakPeer *server, uint32_t maxSimMs = 5000);

		// Deliver raw bytes to a peer as if `from` had sent them, bypassing link
		// rules: for fuzzing what an unconnected remote can send.
		void InjectDatagram(const SystemAddress &from, const RakPeer *to, const unsigned char *data, int length);
		// Called with every datagram any peer sends (before link rules).
		void SetTap(std::function<void(const SystemAddress &from, const SystemAddress &to, const char *data, int length)> tap)
		{
			this->tap = std::move(tap);
		}

		TimeUS Now() const;
		uint64_t DatagramsSent() const { return datagramsSent; }
		uint64_t DatagramsDropped() const { return datagramsDropped; }

		// RakNetSocket2Factory
		RakNetSocket2 *Create(const SocketDescriptor &descriptor, unsigned int index) override;
		void Destroy(RakNetSocket2 *socket) override;

	private:
		class SimSocket;
		friend class SimSocket;

		struct InFlight
		{
			TimeUS deliverAt;
			uint64_t sequence;
			SystemAddress from;
			SystemAddress to;
			std::vector<char> bytes;
		};

		void Enqueue(const SystemAddress &from, const SystemAddress &to, const char *data, int length);
		void DeliverDue();
		RakPeer *StartPeer(RakPeer *peer, unsigned int maxConnections, unsigned short maxIncoming, unsigned short port);
		const LinkRule &RuleFor(const SystemAddress &from, const SystemAddress &to) const;
		uint32_t NextRandom();
		static TimeUS VirtualClock();

		std::vector<RakPeer *> peers;
		std::set<const RakPeer *> peersInStorage;
		std::map<const RakPeer *, std::vector<Message> > inboxes;
		std::set<const RakPeer *> receiveDisabled;
		std::map<SystemAddress, SimSocket *> sockets;
		std::map<std::pair<SystemAddress, SystemAddress>, LinkRule> links;
		LinkRule defaultRule;
		std::vector<InFlight> inFlight;
		uint64_t nextSequence = 0;
		uint64_t datagramsSent = 0;
		uint64_t datagramsDropped = 0;
		uint32_t lcgState;
		std::function<void(const SystemAddress &, const SystemAddress &, const char *, int)> tap;
		unsigned short nextHost = 1;
	};
}
