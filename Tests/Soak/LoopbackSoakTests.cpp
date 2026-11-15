/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Soak test over real UDP on loopback: one server and 64 threaded, encrypted
 *  client peers on OS-assigned ports for minutes of wall-clock time, with
 *  mixed traffic and constant churn. What the simulated soak cannot show: the
 *  network threads, the real socket layer (batched recvmmsg / sendmmsg on
 *  Linux), and Connect() racing the network thread. Cross-connects here are
 *  real: two threads call Connect() on the two peers of a pair at once.
 *
 *  Knob (environment): MAFIANET_SOAK_LOOPBACK_SECONDS (default 150).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "SoakSupport.h"
#include "TestHelpers.h"

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;

namespace
{
const int kClients = 64;
const int kCrossPairs = 16; // clients 1..32
const int kServer = 0;
const TimeMS kTimeoutMs = 5000;

struct Link
{
	enum State
	{
		Idle,
		Connecting,
		Connected
	};
	State state = Idle;
	bool wanted = false;
	TimeMS nextAttemptMs = 0;
	TimeMS connectingSinceMs = 0;
	TimeMS connectedAtMs = 0;
	int64_t settledDrops = -1;
	Soak::SendStream send;
};

struct Node
{
	RakPeerInterface *peer = nullptr;
	SystemAddress address; // 127.0.0.1:<bound port>
	int partner = -1;
	std::map<int, Link> links;
};

class LoopbackSoak : public ::testing::Test
{
protected:
	void TearDown() override
	{
		for (RakPeerInterface *peer : owned)
			peer->Shutdown(100);
		for (RakPeerInterface *peer : owned)
			RakPeerInterface::DestroyInstance(peer);
		owned.clear();
	}

	RakPeerInterface *NewPeer(unsigned int maxConnections, unsigned short maxIncoming, SystemAddress *address)
	{
		RakPeerInterface *peer = RakPeerInterface::GetInstance();
		owned.push_back(peer);
		const unsigned short port = TestHelpers::StartOnLoopback(peer, maxConnections, maxIncoming);
		if (port == 0)
			return nullptr;
		peer->SetTimeoutTime(kTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
		*address = SystemAddress("127.0.0.1", port);
		return peer;
	}

	void Destroy(RakPeerInterface *peer)
	{
		owned.erase(std::find(owned.begin(), owned.end(), peer));
		peer->Shutdown(0); // no notification: a crash
		RakPeerInterface::DestroyInstance(peer);
	}

	std::vector<RakPeerInterface *> owned;
};

struct Counters
{
	uint64_t connections = 0;
	uint64_t orderedSent = 0;
	uint64_t splitSent = 0;
	uint64_t unreliableSent = 0;
	uint64_t unreliableDelivered = 0;
	uint64_t crashes = 0;
	uint64_t reconnects = 0;
	uint64_t absences = 0;
	uint64_t crossRounds = 0;
	uint64_t failedAttempts = 0;
	uint64_t failuresWhileConnected = 0;
	uint64_t staleDrops = 0;
};
} // namespace

TEST_F(LoopbackSoak, ThreadedPeersUnderTrafficChurnAndRealCrossConnects)
{
	const TimeMS durationMs = Soak::EnvUInt("MAFIANET_SOAK_LOOPBACK_SECONDS", 150) * 1000u;
	uint32_t rng = 0x5EED1234u;
	auto random = [&rng]
	{
		rng = rng * 1664525u + 1013904223u;
		return rng >> 8;
	};

	std::vector<Node> nodes(1 + kClients);
	std::map<RakNetGUID, int> byGuid;           // every GUID ever used: never reused
	std::map<SystemAddress, int> byAddress;     // current addresses
	std::map<std::string, uint64_t> violations; // must all stay zero
	std::vector<std::string> examples;
	Soak::OrderChecker checker;
	Counters counters;
	auto violation = [&](const std::string &kind, const std::string &detail)
	{
		++violations[kind];
		if (examples.size() < 40)
			examples.push_back(kind + ": " + detail);
	};

	nodes[kServer].peer = NewPeer(kClients + 16, kClients + 16, &nodes[kServer].address);
	ASSERT_NE(nullptr, nodes[kServer].peer);
	for (int i = 1; i <= kClients; ++i)
	{
		nodes[(size_t) i].peer = NewPeer(4, 1, &nodes[(size_t) i].address);
		ASSERT_NE(nullptr, nodes[(size_t) i].peer);
	}
	for (int p = 0; p < kCrossPairs; ++p)
	{
		nodes[(size_t) (1 + 2 * p)].partner = 2 + 2 * p;
		nodes[(size_t) (2 + 2 * p)].partner = 1 + 2 * p;
	}
	auto reindex = [&](int i)
	{
		byGuid[nodes[(size_t) i].peer->GetMyGUID()] = i;
		byAddress[nodes[(size_t) i].address] = i;
	};
	for (int i = 0; i <= kClients; ++i)
		reindex(i);
	auto identify = [&](const Packet *packet)
	{
		auto g = byGuid.find(packet->guid);
		if (g != byGuid.end() && packet->guid != UNASSIGNED_RAKNET_GUID)
			return g->second;
		auto a = byAddress.find(packet->systemAddress);
		return a == byAddress.end() ? -1 : a->second;
	};
	auto name = [](int i)
	{ return i == kServer ? std::string("server") : "client" + std::to_string(i); };

	auto startConnect = [&](int self, int remote, TimeMS now)
	{
		Link &link = nodes[(size_t) self].links[remote];
		const ConnectionAttemptResult result = nodes[(size_t) self].peer->Connect("127.0.0.1", nodes[(size_t) remote].address.GetPort(), nullptr, 0);
		if (result == CONNECTION_ATTEMPT_STARTED || result == CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS)
		{
			link.state = Link::Connecting;
			link.connectingSinceMs = now;
		}
		else
			link.nextAttemptMs = now + 500;
		return result;
	};

	// Two threads call Connect() on the two peers of a pair at the same instant, each racing the other's
	// network thread (and its own) for the incoming REQUEST_2.
	auto crossConnect = [&](int a, int b, TimeMS now)
	{
		std::atomic<int> ready{0};
		ConnectionAttemptResult resultA = INVALID_PARAMETER, resultB = INVALID_PARAMETER;
		const unsigned short portA = nodes[(size_t) a].address.GetPort(), portB = nodes[(size_t) b].address.GetPort();
		RakPeerInterface *peerA = nodes[(size_t) a].peer, *peerB = nodes[(size_t) b].peer;
		std::thread ta([&]
		               {
			               ++ready;
			               while (ready.load() < 2)
			               {
			               }
			               resultA = peerA->Connect("127.0.0.1", portB, nullptr, 0); });
		std::thread tb([&]
		               {
			               ++ready;
			               while (ready.load() < 2)
			               {
			               }
			               resultB = peerB->Connect("127.0.0.1", portA, nullptr, 0); });
		ta.join();
		tb.join();
		for (auto side : {std::make_pair(std::make_pair(a, b), resultA), std::make_pair(std::make_pair(b, a), resultB)})
		{
			Link &link = nodes[(size_t) side.first.first].links[side.first.second];
			if (side.second == CONNECTION_ATTEMPT_STARTED || side.second == CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS)
			{
				link.state = Link::Connecting;
				link.connectingSinceMs = now;
			}
			else
				link.nextAttemptMs = now + 500;
		}
		++counters.crossRounds;
	};

	auto onConnected = [&](int self, int remote, TimeMS now)
	{
		Link &link = nodes[(size_t) self].links[remote];
		if (link.state == Link::Connected)
			violation("second connection notification for a live connection", name(self) + " <- " + name(remote));
		link.state = Link::Connected;
		link.connectedAtMs = now;
		link.settledDrops = -1;
		link.send.live = true;
		link.send.session = ((uint64_t) random() << 40) ^ ((uint64_t) random() << 16) ^ random() ^ 1;
		link.send.next = 0;
		++counters.connections;
	};
	auto onFailed = [&](int self, int remote, TimeMS now)
	{
		Link &link = nodes[(size_t) self].links[remote];
		++counters.failedAttempts;
		if (link.state == Link::Connected)
		{
			++counters.failuresWhileConnected;
			return;
		}
		link.state = Link::Idle;
		link.nextAttemptMs = now + 200 + random() % 600;
	};
	auto onLost = [&](int self, int remote, TimeMS now)
	{
		Link &link = nodes[(size_t) self].links[remote];
		link.state = Link::Idle;
		link.send.live = false;
		link.nextAttemptMs = now + 200 + random() % 500;
	};

	auto pump = [&](TimeMS now)
	{
		for (int i = 0; i <= kClients; ++i)
		{
			RakPeerInterface *peer = nodes[(size_t) i].peer;
			for (Packet *packet = peer->Receive(); packet != nullptr; peer->DeallocatePacket(packet), packet = peer->Receive())
			{
				const int remote = identify(packet);
				const bool current = remote >= 0 && byAddress.count(packet->systemAddress) != 0 && byAddress[packet->systemAddress] == remote;
				switch (packet->data[0])
				{
				case ID_CONNECTION_REQUEST_ACCEPTED:
				case ID_NEW_INCOMING_CONNECTION:
					if (!current)
						violation("connection from an unknown address", name(i) + " " + packet->systemAddress.ToString());
					else
						onConnected(i, remote, now);
					break;
				case ID_CONNECTION_ATTEMPT_FAILED:
				case ID_ALREADY_CONNECTED:
				case ID_NO_FREE_INCOMING_CONNECTIONS:
				case ID_IP_RECENTLY_CONNECTED:
					if (current)
						onFailed(i, remote, now);
					break;
				case ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION:
				case ID_OUR_SYSTEM_REQUIRES_SECURITY:
				case ID_INCOMPATIBLE_PROTOCOL_VERSION:
					violation("refused between two encrypted peers", name(i) + " from " + name(remote));
					if (current)
						onFailed(i, remote, now);
					break;
				case ID_CONNECTION_LOST:
				case ID_DISCONNECTION_NOTIFICATION:
					// From a crashed peer's old address the link already moved on.
					if (current)
						onLost(i, remote, now);
					break;
				case Soak::ID_SOAK_ORDERED:
					if (current)
						checker.OnOrdered(i, remote, packet->data, packet->length);
					break;
				case Soak::ID_SOAK_UNRELIABLE:
					++counters.unreliableDelivered;
					break;
				default:
					break;
				}
			}
		}
	};

	auto settle = [&](TimeMS now)
	{
		for (int i = 0; i <= kClients; ++i)
			for (auto &entry : nodes[(size_t) i].links)
			{
				Link &link = entry.second;
				if (link.state != Link::Connected || link.settledDrops >= 0 || now < link.connectedAtMs + 2000)
					continue;
				RakNetStatistics stats;
				if (nodes[(size_t) i].peer->GetStatistics(nodes[(size_t) entry.first].address, &stats) == nullptr)
					continue;
				link.settledDrops = (int64_t) stats.droppedUnauthenticatedDatagrams;
				counters.staleDrops += stats.droppedUnauthenticatedDatagrams;
			}
	};

	auto drive = [&](TimeMS now)
	{
		for (int i = 1; i <= kClients; ++i)
		{
			Link &toServer = nodes[(size_t) i].links[kServer];
			if (toServer.wanted && toServer.state == Link::Idle && now >= toServer.nextAttemptMs)
				startConnect(i, kServer, now);
		}
		for (int p = 0; p < kCrossPairs; ++p)
		{
			const int a = 1 + 2 * p, b = 2 + 2 * p;
			Link &ab = nodes[(size_t) a].links[b];
			Link &ba = nodes[(size_t) b].links[a];
			if (ab.wanted && ba.wanted && ab.state == Link::Idle && ba.state == Link::Idle && now >= std::max(ab.nextAttemptMs, ba.nextAttemptMs))
				crossConnect(a, b, now);
		}
		for (int i = 0; i <= kClients; ++i)
			for (auto &entry : nodes[(size_t) i].links)
				if (entry.second.state == Link::Connecting && now - entry.second.connectingSinceMs > 60000)
				{
					violation("connection attempt never resolved", name(i) + " -> " + name(entry.first));
					entry.second.state = Link::Idle;
				}
	};

	uint64_t tick = 0;
	auto sendTraffic = [&]
	{
		++tick;
		for (int i = 0; i <= kClients; ++i)
			for (auto &entry : nodes[(size_t) i].links)
			{
				Link &link = entry.second;
				if (link.state != Link::Connected || !link.send.live)
					continue;
				const int remote = entry.first;
				const bool cross = i != kServer && remote != kServer;
				const uint64_t phase = tick + (uint64_t) i * 7 + (uint64_t) remote;
				uint16_t extra = i == kServer ? 8 : (cross ? 24 : 16);
				if (phase % (cross ? 40 : 25) == 0)
					extra = (uint16_t) (2500 + (phase % 9) * 300);
				BitStream ordered;
				Soak::WriteOrdered(ordered, link.send.session, link.send.next, extra);
				if (nodes[(size_t) i].peer->Send(&ordered, Priority::Medium, Reliability::ReliableOrdered, 0, nodes[(size_t) remote].address, false) != 0)
				{
					++link.send.next;
					++counters.orderedSent;
					counters.splitSent += extra > 1000;
				}
				if (i != kServer && !cross)
				{
					BitStream unreliable;
					unreliable.Write(Soak::ID_SOAK_UNRELIABLE);
					unreliable.Write((uint32_t) tick);
					if (nodes[(size_t) i].peer->Send(&unreliable, Priority::Medium, Reliability::Unreliable, 1, nodes[(size_t) remote].address, false) != 0)
						++counters.unreliableSent;
				}
			}
	};

	auto allConnected = [&]
	{
		for (int i = 1; i <= kClients; ++i)
			for (const auto &entry : nodes[(size_t) i].links)
			{
				if (!entry.second.wanted)
					continue;
				auto back = nodes[(size_t) entry.first].links.find(i);
				if (entry.second.state != Link::Connected || back == nodes[(size_t) entry.first].links.end() || back->second.state != Link::Connected)
					return false;
			}
		return true;
	};

	auto churn = [&](TimeMS now)
	{
		for (int n = 0; n < 3; ++n)
		{
			const int i = 1 + (int) (random() % kClients);
			Node &node = nodes[(size_t) i];
			const uint32_t roll = random() % 3;
			if (roll == 0)
			{
				// Crash: gone without a word; a new process (new port, new GUID) takes its place.
				byAddress.erase(node.address);
				Destroy(node.peer);
				node.peer = NewPeer(4, 1, &node.address);
				ASSERT_NE(nullptr, node.peer);
				reindex(i);
				for (auto &entry : node.links)
				{
					entry.second.state = Link::Idle;
					entry.second.send.live = false;
					entry.second.nextAttemptMs = now;
				}
				// Whoever was connected to the old process finds out by timeout; until then it still holds
				// the old address, which is no longer this node's.
				for (int j = 0; j <= kClients; ++j)
				{
					auto it = nodes[(size_t) j].links.find(i);
					if (it != nodes[(size_t) j].links.end())
					{
						it->second.state = Link::Idle;
						it->second.send.live = false;
						it->second.nextAttemptMs = now + kTimeoutMs;
					}
				}
				++counters.crashes;
			}
			else if (roll == 1)
			{
				for (auto &entry : node.links)
				{
					if (entry.second.state == Link::Connected)
						node.peer->CloseConnection(nodes[(size_t) entry.first].address, true);
					else if (entry.second.state == Link::Connecting)
						node.peer->CancelConnectionAttempt(nodes[(size_t) entry.first].address);
					entry.second.state = Link::Idle;
					entry.second.send.live = false;
					entry.second.nextAttemptMs = now;
				}
				++counters.reconnects;
			}
			else
			{
				Link &toServer = node.links[kServer];
				if (toServer.state == Link::Connected)
					node.peer->CloseConnection(nodes[kServer].address, true);
				else if (toServer.state == Link::Connecting)
					node.peer->CancelConnectionAttempt(nodes[kServer].address);
				toServer.state = Link::Idle;
				toServer.send.live = false;
				toServer.nextAttemptMs = now + 2000 + random() % 3000;
				++counters.absences;
			}
		}
	};

	// Everyone wants the server; each pair wants each other.
	for (int i = 1; i <= kClients; ++i)
	{
		nodes[(size_t) i].links[kServer].wanted = true;
		nodes[(size_t) i].links[kServer].nextAttemptMs = GetTimeMS() + random() % 500;
		if (nodes[(size_t) i].partner > 0)
			nodes[(size_t) i].links[nodes[(size_t) i].partner].wanted = true;
	}

	const TimeMS start = GetTimeMS();
	TimeMS nextTraffic = start, nextChurn = start + 10000, lastProgress = start;
	bool churning = true;
	while (GetTimeMS() - start < durationMs)
	{
		const TimeMS now = GetTimeMS();
		pump(now);
		settle(now);
		drive(now);
		if (now >= nextTraffic)
		{
			sendTraffic();
			nextTraffic = now + 50;
		}
		if (churning && now >= nextChurn)
		{
			churn(now);
			if (HasFatalFailure())
				return;
			nextChurn = now + 3000;
		}
		if (now - lastProgress >= 30000)
		{
			std::printf("[soak-loopback] %u s: connections %" PRIu64 ", ordered delivered %" PRIu64 ", crashes %" PRIu64 ", cross rounds %" PRIu64 "\n",
			            (unsigned) ((now - start) / 1000), counters.connections, checker.Delivered(), counters.crashes, counters.crossRounds);
			lastProgress = now;
		}
		RakSleep(10);
	}
	churning = false;

	// Everyone back, then quiesce: no new data, everything sent on a live connection arrives.
	const TimeMS backBy = GetTimeMS() + 60000;
	while (GetTimeMS() < backBy && !allConnected())
	{
		const TimeMS now = GetTimeMS();
		pump(now);
		settle(now);
		drive(now);
		RakSleep(10);
	}
	ASSERT_TRUE(allConnected()) << "not every client came back";
	const TimeMS quiesceUntil = GetTimeMS() + 5000;
	while (GetTimeMS() < quiesceUntil)
	{
		const TimeMS now = GetTimeMS();
		pump(now);
		settle(now);
		RakSleep(10);
	}
	for (int i = 0; i <= kClients; ++i)
		for (const auto &entry : nodes[(size_t) i].links)
		{
			const Link &link = entry.second;
			auto back = nodes[(size_t) entry.first].links.find(i);
			if (link.state != Link::Connected || back == nodes[(size_t) entry.first].links.end() || back->second.state != Link::Connected)
				continue;
			if (link.send.next > 0)
			{
				const Soak::RecvStream *stream = checker.Stream(entry.first, i);
				if (stream == nullptr || stream->session != link.send.session || stream->expected != link.send.next)
					violation("not delivered after quiescing", name(i) + " -> " + name(entry.first) + " sent " + std::to_string(link.send.next));
			}
			RakNetStatistics stats;
			if (link.settledDrops < 0)
				violation("connection never settled", name(i) + " -> " + name(entry.first));
			else if (nodes[(size_t) i].peer->GetStatistics(nodes[(size_t) entry.first].address, &stats) != nullptr &&
			         (int64_t) stats.droppedUnauthenticatedDatagrams != link.settledDrops)
				violation("droppedUnauthenticatedDatagrams", name(i) + " from " + name(entry.first));
		}

	// Everybody leaves; every slot is released.
	for (int i = 0; i <= kClients; ++i)
		for (auto &entry : nodes[(size_t) i].links)
		{
			entry.second.wanted = false;
			if (entry.second.state == Link::Connected)
				nodes[(size_t) i].peer->CloseConnection(nodes[(size_t) entry.first].address, true);
			else if (entry.second.state == Link::Connecting)
				nodes[(size_t) i].peer->CancelConnectionAttempt(nodes[(size_t) entry.first].address);
		}
	auto released = [&](std::string *why)
	{
		for (int i = 0; i <= kClients; ++i)
		{
			if (nodes[(size_t) i].peer->NumberOfConnections() != 0)
			{
				*why = name(i) + " has " + std::to_string(nodes[(size_t) i].peer->NumberOfConnections()) + " connections";
				return false;
			}
			for (int j = 0; j <= kClients; ++j)
			{
				const ConnectionState state = j == i ? IS_NOT_CONNECTED : nodes[(size_t) i].peer->GetConnectionState(nodes[(size_t) j].address);
				if (state != IS_NOT_CONNECTED && state != IS_DISCONNECTED)
				{
					*why = name(i) + " holds " + name(j) + " in state " + std::to_string((int) state);
					return false;
				}
			}
		}
		return true;
	};
	std::string why;
	const TimeMS releaseBy = GetTimeMS() + 30000;
	while (GetTimeMS() < releaseBy && !released(&why))
	{
		pump(GetTimeMS());
		RakSleep(30);
	}
	EXPECT_TRUE(released(&why)) << "slot leak: " << why;

	std::printf("[soak-loopback] %u s: connections %" PRIu64 ", ordered sent %" PRIu64 " delivered %" PRIu64 " (split %" PRIu64 "), unreliable sent %" PRIu64
	            " delivered %" PRIu64 "\n",
	            (unsigned) ((GetTimeMS() - start) / 1000), counters.connections, counters.orderedSent, checker.Delivered(), counters.splitSent,
	            counters.unreliableSent, counters.unreliableDelivered);
	std::printf("[soak-loopback] crashes %" PRIu64 ", reconnects %" PRIu64 ", absences %" PRIu64 ", cross-connect rounds %" PRIu64 ", failed attempts %" PRIu64
	            " (%" PRIu64 " after the connection stood), stale drops settled %" PRIu64 "\n",
	            counters.crashes, counters.reconnects, counters.absences, counters.crossRounds, counters.failedAttempts, counters.failuresWhileConnected,
	            counters.staleDrops);
	for (const std::string &e : examples)
		std::printf("[soak-loopback] violation %s\n", e.c_str());
	for (const std::string &e : checker.Examples())
		std::printf("[soak-loopback] order %s\n", e.c_str());
	for (const auto &v : violations)
		ADD_FAILURE() << v.first << ": " << v.second;
	EXPECT_EQ(0u, checker.Violations()) << "ordering violations";
	EXPECT_GT(counters.crossRounds, (uint64_t) kCrossPairs);
	EXPECT_GT(counters.crashes, 0u);
	EXPECT_GT(counters.splitSent, 0u);
}
