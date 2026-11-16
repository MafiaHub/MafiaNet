/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Soak test on the SimulatedNetwork: one server and hundreds of encrypted
 *  clients over many minutes of virtual time. A seed fixes the scenario (link
 *  rules, churn, timing); GUIDs and keys still come from the OS, so which side
 *  of a cross-connect wins, and the timing that follows, varies between runs.
 *
 *  Every client's link to the server has its own loss (5-20%), latency and
 *  jitter (so datagrams reorder), and some are capped below the MTU. Pairs of
 *  clients connect to each other at the same instant (cross-connects). A few
 *  plaintext "rogue" peers keep trying to connect to encrypted ones. Each wave:
 *
 *    1. every client connects (a reconnect storm from the second wave on);
 *    2. sustained traffic: reliable-ordered (some split), reliable (some over the 1280 rung, some split;
 *       each must arrive exactly once), unreliable;
 *    3. a churn storm under traffic: clients disconnect and reconnect at once,
 *       leave for a while, or crash (destroyed without a word) and come back
 *       from the same address with a new GUID;
 *    4. quiesce: everything sent on a surviving connection must arrive;
 *    5. everybody disconnects; every slot must be released.
 *
 *  Knobs (environment): MAFIANET_SOAK_SEED (first seed, default 1),
 *  MAFIANET_SOAK_ITERATIONS (seeds to run, default 1), MAFIANET_SOAK_CLIENTS
 *  (default 200), MAFIANET_SOAK_WAVES (default 8), MAFIANET_SOAK_PHASE_SECONDS
 *  (traffic seconds per phase, default 30). Many short waves rather than a few
 *  long ones: a leak shows up per reconnect, so the memory check needs waves.
 *
 *  No link here black-holes large datagrams: the capped links are capped in both
 *  directions, so the handshake already negotiates an MTU that fits. Every MTU
 *  step-down is therefore a false positive of the black-hole detector (#94); the
 *  run reports them, and the Reliable stream catches a step-down's re-sent copy
 *  delivered twice (#93).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "SimulatedNetwork.h"
#include "SoakSupport.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/session_crypto.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;

namespace
{
const int kServer = 0;
const uint32_t kTickMs = 100;
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
	uint64_t nextAttemptMs = 0;
	uint64_t connectingSinceMs = 0;
	uint64_t connectedAtMs = 0;
	// droppedUnauthenticatedDatagrams once the connection has settled (see SettleBaselines); -1 until then.
	int64_t settledDrops = -1;
	// No earlier connection between these two addresses: no stale datagram can exist.
	bool firstBetweenAddresses = false;
	Soak::SendStream send;
	// The Reliable (unordered) stream shares the session of `send`. It starts with the connection, so
	// its first messages can overtake the connecting side's session reply (issue #96).
	uint32_t nextReliable = 0;
	// RakNetStatistics::mtuStepDowns already added to Totals for this connection.
	uint32_t stepDownsCounted = 0;
};

struct Node
{
	RakPeer *peer = nullptr;
	SystemAddress address;
	bool rogue = false;
	int partner = -1;                      // cross-connect partner, or -1
	SimulatedNetwork::LinkRule serverLink; // clients: the rule of their link to the server
	std::map<int, Link> links;
};

// Counters that must stay zero, each with the first few examples.
struct Violations
{
	std::map<std::string, uint64_t> counts;
	std::vector<std::string> examples;

	void Add(const std::string &kind, const std::string &detail)
	{
		++counts[kind];
		if (examples.size() < 40)
			examples.push_back(kind + ": " + detail);
	}
	uint64_t Total() const
	{
		uint64_t n = 0;
		for (const auto &c : counts)
			n += c.second;
		return n;
	}
};

struct Totals
{
	uint64_t connectionsCompleted = 0;
	uint64_t orderedSent = 0;
	uint64_t reliableSent = 0;
	uint64_t unreliableSent = 0;
	uint64_t unreliableDelivered = 0;
	uint64_t splitSent = 0;
	uint64_t rogueRefusals = 0;
	uint64_t crashes = 0;
	uint64_t gracefulReconnects = 0;
	uint64_t absences = 0;
	uint64_t crossConnectRounds = 0;
	uint64_t failedAttempts = 0;
	uint64_t failuresWhileConnected = 0;
	uint64_t staleDropsExcused = 0;
	uint64_t linksWithStaleDrops = 0;
	uint64_t mtuStepDowns = 0;
	uint64_t mtuStepDownsOnCappedLinks = 0;
};

// Pages in a peer's packet pool. Never instantiated: the derived type only names the protected member, read
// through a pointer-to-member on the real RakPeer.
class PacketPoolInspector : public RakPeer
{
public:
	static const DataStructures::MemoryPool<Packet> &Pool(const RakPeer *peer) { return peer->*&PacketPoolInspector::packetAllocationPool; }
	static int64_t Pages(const RakPeer *peer) { return (int64_t) Pool(peer).GetAvailablePagesSize() + Pool(peer).GetUnavailablePagesSize(); }
};
// MemoryPool::Allocate and InitPage take three rakMalloc blocks per page: the page header, its block array
// and its free-block stack.
const int64_t kRakBlocksPerPoolPage = 3;

class SimulatedSoak
{
public:
	SimulatedSoak(uint32_t seed, int clients, int waves, uint32_t phaseSeconds)
	    : net(seed), rng(seed * 2654435761u + 12345u), clientCount(clients), waveCount(waves), phaseMs(phaseSeconds * 1000u)
	{
	}

	void Run();

private:
	uint32_t Random()
	{
		rng = rng * 1664525u + 1013904223u;
		return rng >> 8;
	}
	uint64_t Random64() { return ((uint64_t) Random() << 40) ^ ((uint64_t) Random() << 16) ^ Random(); }
	uint64_t NowMs() const { return net.Now() / 1000; }

	void Build();
	RakPeer *Configure(RakPeer *peer);
	int IndexOf(const SystemAddress &address) const
	{
		auto it = byAddress.find(address);
		return it == byAddress.end() ? -1 : it->second;
	}
	std::string Name(int i) const
	{
		return i == kServer ? std::string("server") : (nodes[(size_t) i].rogue ? "rogue" : "client") + std::to_string(i);
	}

	void Tick(bool traffic);
	void RunFor(uint64_t ms, bool traffic);
	bool RunUntil(const std::function<bool()> &done, uint64_t maxMs, bool traffic);
	void ProcessInboxes();
	void OnConnected(int self, int remote);
	void OnAttemptFailed(int self, int remote);
	void OnLost(int self, int remote);
	void DriveConnections();
	bool StartConnect(int self, int remote);
	void SendTraffic();
	void SettleBaselines();
	void CountStepDowns();
	bool AllWantedConnected() const;

	void Storm();
	void CheckQuiescedDelivery(int wave);
	void DisconnectEveryone();
	bool EverythingReleased(std::string *why) const;
	std::string Describe(int from, int to);

	SimulatedNetwork net;
	uint32_t rng;
	int clientCount;
	int waveCount;
	uint64_t phaseMs;
	std::vector<Node> nodes; // [0] server, [1..clientCount] clients, then rogues
	std::map<SystemAddress, int> byAddress;
	std::vector<std::pair<int, int>> crossPairs;
	std::map<std::pair<int, int>, uint64_t> crossNextAttempt;
	std::set<std::pair<int, int>> connectedPairs; // per direction (self, remote)
	Soak::OrderChecker checker;
	Soak::ExactlyOnceChecker reliableChecker;
	Violations violations;
	Totals totals;
	uint64_t tickCount = 0;
};

RakPeer *SimulatedSoak::Configure(RakPeer *peer)
{
	peer->SetTimeoutTime(kTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS);
	return peer;
}

void SimulatedSoak::Build()
{
	const int rogues = 8;
	const int crossPairCount = clientCount >= 64 ? 16 : clientCount / 4;

	Node server;
	server.peer = Configure(net.AddPeer((unsigned) (clientCount + rogues + 16), (unsigned short) (clientCount + rogues + 16)));
	server.address = net.AddressOf(server.peer);
	nodes.push_back(server);

	SimulatedNetwork::LinkRule defaultRule;
	defaultRule.lossPercent = 5;
	defaultRule.delayMs = 10;
	defaultRule.jitterMs = 20;
	net.SetDefaultLink(defaultRule);

	const int caps[] = {548, 1000, 1200};
	for (int i = 1; i <= clientCount; ++i)
	{
		Node client;
		client.peer = Configure(net.AddPeer(4, 1));
		client.address = net.AddressOf(client.peer);
		SimulatedNetwork::LinkRule rule;
		rule.lossPercent = 5 + (int) (Random() % 16);
		rule.delayMs = 5 + Random() % 50;
		rule.jitterMs = Random() % 40;
		if (Random() % 100 < 15)
			rule.maxDatagramBytes = caps[Random() % 3];
		net.SetLinkBothWays(client.peer, server.peer, rule);
		client.serverLink = rule;
		nodes.push_back(client);
	}
	for (int p = 0; p < crossPairCount; ++p)
	{
		const int a = 1 + 2 * p, b = 2 + 2 * p;
		nodes[(size_t) a].partner = b;
		nodes[(size_t) b].partner = a;
		crossPairs.push_back({a, b});
	}

	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	for (int r = 0; r < rogues; ++r)
	{
		Node rogue;
		rogue.peer = Configure(net.AddPeer(2, 0));
		rogue.address = net.AddressOf(rogue.peer);
		rogue.rogue = true;
		nodes.push_back(rogue);
	}
	// RestartPeer starts the replacement in the current mode: every crash victim is an encrypted client.
	net.SetNewPeerEncryptionMode(EncryptionMode::Required);

	for (size_t i = 0; i < nodes.size(); ++i)
		byAddress[nodes[i].address] = (int) i;
}

void SimulatedSoak::Tick(bool traffic)
{
	DriveConnections();
	net.Step(kTickMs);
	++tickCount;
	ProcessInboxes();
	SettleBaselines();
	if (traffic)
		SendTraffic();
}

// A datagram the previous connection between the same two addresses had in flight (a resend, an ack, a
// keep-alive) can arrive just after the new one is keyed. It cannot be opened under the new keys and is
// counted in droppedUnauthenticatedDatagrams, as it should be. Such datagrams are all at most one link
// latency (< 100 ms here) old: a new connection to an address only starts once the old one is gone on
// both ends. So the counter is read 2 s into each connection, and from then on it must not move.
void SimulatedSoak::SettleBaselines()
{
	const uint64_t now = NowMs();
	for (size_t i = 0; i < nodes.size(); ++i)
		for (auto &entry : nodes[i].links)
		{
			Link &link = entry.second;
			if (link.state != Link::Connected || link.settledDrops >= 0 || now < link.connectedAtMs + 2000)
				continue;
			RakNetStatistics stats;
			if (nodes[i].peer->GetStatistics(nodes[(size_t) entry.first].address, &stats) == nullptr)
				continue;
			link.settledDrops = (int64_t) stats.droppedUnauthenticatedDatagrams;
			if (link.firstBetweenAddresses && link.settledDrops != 0)
				violations.Add("droppedUnauthenticatedDatagrams on a first connection", Name((int) i) + " from " + Name(entry.first));
			totals.staleDropsExcused += stats.droppedUnauthenticatedDatagrams;
			totals.linksWithStaleDrops += stats.droppedUnauthenticatedDatagrams != 0;
		}
}

void SimulatedSoak::RunFor(uint64_t ms, bool traffic)
{
	for (uint64_t t = 0; t < ms; t += kTickMs)
		Tick(traffic);
}

bool SimulatedSoak::RunUntil(const std::function<bool()> &done, uint64_t maxMs, bool traffic)
{
	for (uint64_t t = 0; t < maxMs; t += kTickMs)
	{
		if (done())
			return true;
		Tick(traffic);
	}
	return done();
}

void SimulatedSoak::ProcessInboxes()
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		std::vector<SimulatedNetwork::Message> &inbox = net.Inbox(nodes[i].peer);
		for (const SimulatedNetwork::Message &message : inbox)
		{
			const int self = (int) i;
			const int remote = IndexOf(message.systemAddress);
			switch (message.id)
			{
			case ID_CONNECTION_REQUEST_ACCEPTED:
			case ID_NEW_INCOMING_CONNECTION:
				OnConnected(self, remote);
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
			case ID_ALREADY_CONNECTED:
			case ID_NO_FREE_INCOMING_CONNECTIONS:
			case ID_IP_RECENTLY_CONNECTED:
			case ID_CONNECTION_BANNED:
			case ID_INCOMPATIBLE_PROTOCOL_VERSION:
				OnAttemptFailed(self, remote);
				break;
			case ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION:
			case ID_OUR_SYSTEM_REQUIRES_SECURITY:
				if (nodes[i].rogue)
					++totals.rogueRefusals;
				else
					violations.Add("mode mismatch between encrypted peers", Name(self) + " from " + Name(remote));
				OnAttemptFailed(self, remote);
				break;
			case ID_CONNECTION_LOST:
			case ID_DISCONNECTION_NOTIFICATION:
				OnLost(self, remote);
				break;
			case Soak::ID_SOAK_ORDERED:
				if (remote < 0)
					violations.Add("data from an unknown address", Name(self));
				else
				{
					checker.OnOrdered(self, remote, message.data.data(), message.data.size());
				}
				break;
			case Soak::ID_SOAK_RELIABLE:
				if (remote < 0)
					violations.Add("data from an unknown address", Name(self));
				else
					reliableChecker.OnReliable(self, remote, message.data.data(), message.data.size());
				break;
			case Soak::ID_SOAK_UNRELIABLE:
				++totals.unreliableDelivered;
				break;
			default:
				break;
			}
		}
		inbox.clear();
	}
}

void SimulatedSoak::OnConnected(int self, int remote)
{
	if (remote < 0)
	{
		violations.Add("connection from an unknown address", Name(self));
		return;
	}
	if (nodes[(size_t) self].rogue || nodes[(size_t) remote].rogue)
	{
		violations.Add("plaintext peer connected", Name(self) + " <-> " + Name(remote));
		return;
	}
	Link &link = nodes[(size_t) self].links[remote];
	if (link.state == Link::Connected)
		violations.Add("second connection notification for a live connection", Name(self) + " <- " + Name(remote));

	// Every connection is encrypted, and both ends hold the two halves of one key pair.
	SessionKeys mine, theirs;
	if (!nodes[(size_t) self].peer->GetSessionKeysForTesting(nodes[(size_t) remote].address, &mine))
		violations.Add("connection without session keys", Name(self) + " <-> " + Name(remote));
	else if (nodes[(size_t) remote].peer->GetSessionKeysForTesting(nodes[(size_t) self].address, &theirs))
	{
		if (std::memcmp(mine.send, theirs.recv, sizeof(mine.send)) != 0 || std::memcmp(mine.recv, theirs.send, sizeof(mine.recv)) != 0)
			violations.Add("the two ends hold keys from different handshakes", Name(self) + " <-> " + Name(remote));
	}

	link.state = Link::Connected;
	link.connectedAtMs = NowMs();
	link.firstBetweenAddresses = connectedPairs.insert({self, remote}).second;
	link.settledDrops = -1;
	link.send.live = true;
	link.send.session = Random64() | 1;
	link.send.next = 0;
	link.nextReliable = 0;
	link.stepDownsCounted = 0;
	++totals.connectionsCompleted;
}

void SimulatedSoak::OnAttemptFailed(int self, int remote)
{
	if (remote < 0)
		return;
	Link &link = nodes[(size_t) self].links[remote];
	++totals.failedAttempts;
	if (link.state == Link::Connected)
	{
		// A late refusal for an attempt that a cross-connect already resolved: the connection stands.
		++totals.failuresWhileConnected;
		return;
	}
	link.state = Link::Idle;
	link.nextAttemptMs = NowMs() + 300 + Random() % 700;
}

void SimulatedSoak::OnLost(int self, int remote)
{
	if (remote < 0)
		return;
	Link &link = nodes[(size_t) self].links[remote];
	link.state = Link::Idle;
	link.send.live = false;
	link.nextAttemptMs = NowMs() + 200 + Random() % 500;
}

bool SimulatedSoak::StartConnect(int self, int remote)
{
	Link &link = nodes[(size_t) self].links[remote];
	char host[64];
	nodes[(size_t) remote].address.ToString(false, host, sizeof(host));
	const ConnectionAttemptResult result = nodes[(size_t) self].peer->Connect(host, nodes[(size_t) remote].address.GetPort(), nullptr, 0);
	if (result == CONNECTION_ATTEMPT_STARTED || result == CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS)
	{
		link.state = Link::Connecting;
		link.connectingSinceMs = NowMs();
		return true;
	}
	// ALREADY_CONNECTED_TO_ENDPOINT while an old connection is still closing: try again later.
	link.nextAttemptMs = NowMs() + 500;
	return false;
}

void SimulatedSoak::DriveConnections()
{
	const uint64_t now = NowMs();
	for (size_t i = 1; i < nodes.size(); ++i)
	{
		Node &node = nodes[i];
		if (node.rogue)
		{
			// A plaintext peer knocks on the server or an encrypted client every few seconds.
			if (tickCount % 30 == i % 30)
			{
				// A fixed set of targets per rogue, so the bookkeeping here does not grow with time.
				const int target = Random() % 2 == 0 ? kServer : 1 + (int) ((i * 13 + Random() % 4) % (uint32_t) clientCount);
				Link &link = node.links[target];
				if (link.state == Link::Idle)
					StartConnect((int) i, target);
			}
			continue;
		}
		Link &toServer = node.links[kServer];
		if (toServer.wanted && toServer.state == Link::Idle && now >= toServer.nextAttemptMs)
			StartConnect((int) i, kServer);
	}
	// Cross-connects: both ends call Connect in the same instant.
	for (const auto &pair : crossPairs)
	{
		Link &ab = nodes[(size_t) pair.first].links[pair.second];
		Link &ba = nodes[(size_t) pair.second].links[pair.first];
		uint64_t &next = crossNextAttempt[pair];
		if (ab.wanted && ba.wanted && ab.state == Link::Idle && ba.state == Link::Idle && now >= std::max(next, std::max(ab.nextAttemptMs, ba.nextAttemptMs)))
		{
			StartConnect(pair.first, pair.second);
			StartConnect(pair.second, pair.first);
			next = now + 1000;
			++totals.crossConnectRounds;
		}
	}
	// An attempt always resolves: success, refusal or ID_CONNECTION_ATTEMPT_FAILED.
	for (size_t i = 0; i < nodes.size(); ++i)
		for (auto &entry : nodes[i].links)
			if (entry.second.state == Link::Connecting && now - entry.second.connectingSinceMs > 60000)
			{
				violations.Add("connection attempt never resolved", Name((int) i) + " -> " + Name(entry.first) + " (state " +
				                                                        std::to_string((int) nodes[i].peer->GetConnectionState(nodes[(size_t) entry.first].address)) + ")");
				entry.second.state = Link::Idle;
			}
}

void SimulatedSoak::SendTraffic()
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		Node &node = nodes[i];
		for (auto &entry : node.links)
		{
			Link &link = entry.second;
			if (link.state != Link::Connected || !link.send.live)
				continue;
			const int remote = entry.first;
			const bool cross = i != kServer && remote != kServer;
			const uint64_t phase = tickCount + i * 7 + (uint64_t) remote;
			if (cross && phase % 2 != 0)
				continue;
			uint16_t extra = i == kServer ? 8 : (cross ? 24 : 16);
			const uint64_t splitEvery = i == kServer ? 30 : (cross ? 40 : 20);
			if (phase % splitEvery == 0)
				extra = (uint16_t) (2500 + (phase % 7) * 100);

			BitStream ordered;
			Soak::WriteOrdered(ordered, link.send.session, link.send.next, extra);
			if (node.peer->Send(&ordered, Priority::Medium, Reliability::ReliableOrdered, 0, nodes[(size_t) remote].address, false) != 0)
			{
				++link.send.next;
				++totals.orderedSent;
				totals.splitSent += extra > 1000;
			}
			if (!cross)
			{
				// Every few seconds a message over the 1280 rung in one datagram, or split in two: the
				// sizes a black-hole step-down re-sends.
				uint16_t reliableExtra = 24;
				if (phase % 25 == 3)
					reliableExtra = 1300;
				else if (phase % 25 == 15)
					reliableExtra = 2600;
				BitStream reliable;
				Soak::WriteReliable(reliable, link.send.session, link.nextReliable, reliableExtra);
				if (node.peer->Send(&reliable, Priority::Medium, Reliability::Reliable, 0, nodes[(size_t) remote].address, false) != 0)
				{
					++link.nextReliable;
					++totals.reliableSent;
				}
			}
			if (i != kServer && !cross)
			{
				BitStream unreliable;
				unreliable.Write(Soak::ID_SOAK_UNRELIABLE);
				unreliable.Write((uint32_t) tickCount);
				for (int b = 0; b < 32; ++b)
					unreliable.Write((unsigned char) b);
				if (node.peer->Send(&unreliable, Priority::Medium, Reliability::Unreliable, 1, nodes[(size_t) remote].address, false) != 0)
					++totals.unreliableSent;
			}
		}
	}
}

// Adds each live connection's new MTU step-downs to the totals. Called before connections are torn down;
// a crash between two calls loses that connection's step-downs since the first.
void SimulatedSoak::CountStepDowns()
{
	for (size_t i = 0; i < nodes.size(); ++i)
		for (auto &entry : nodes[i].links)
		{
			Link &link = entry.second;
			RakNetStatistics stats;
			if (link.state != Link::Connected || nodes[i].peer->GetStatistics(nodes[(size_t) entry.first].address, &stats) == nullptr)
				continue;
			if (stats.mtuStepDowns <= link.stepDownsCounted)
				continue;
			const uint32_t added = stats.mtuStepDowns - link.stepDownsCounted;
			link.stepDownsCounted = stats.mtuStepDowns;
			totals.mtuStepDowns += added;
			const int client = i == kServer ? entry.first : (int) i;
			const bool toServer = i == kServer || entry.first == kServer;
			if (toServer && nodes[(size_t) client].serverLink.maxDatagramBytes < MAXIMUM_MTU_SIZE)
				totals.mtuStepDownsOnCappedLinks += added;
		}
}

bool SimulatedSoak::AllWantedConnected() const
{
	for (size_t i = 1; i < nodes.size(); ++i)
	{
		const Node &node = nodes[i];
		if (node.rogue)
			continue;
		for (const auto &entry : node.links)
		{
			if (!entry.second.wanted)
				continue;
			if (entry.second.state != Link::Connected)
				return false;
			// The other end must agree.
			auto back = nodes[(size_t) entry.first].links.find((int) i);
			if (back == nodes[(size_t) entry.first].links.end() || back->second.state != Link::Connected)
				return false;
		}
	}
	return true;
}

void SimulatedSoak::Storm()
{
	const uint64_t now = NowMs();
	for (int i = 1; i <= clientCount; ++i)
	{
		Node &node = nodes[(size_t) i];
		const uint32_t roll = Random() % 100;
		if (roll < 15)
		{
			// Crash: destroyed without a word, back at once from the same address with a new GUID.
			node.peer = Configure(net.RestartPeer(node.peer, 4, 1));
			if (net.AddressOf(node.peer) != node.address)
				violations.Add("restarted peer moved", Name(i));
			for (auto &entry : node.links)
			{
				entry.second.state = Link::Idle;
				entry.second.send.live = false;
				entry.second.nextAttemptMs = now;
			}
			++totals.crashes;
		}
		else if (roll < 35)
		{
			// Disconnect and reconnect at once.
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
			++totals.gracefulReconnects;
		}
		else if (roll < 45)
		{
			// Leave the server for a while.
			Link &toServer = node.links[kServer];
			if (toServer.state == Link::Connected)
				node.peer->CloseConnection(nodes[kServer].address, true);
			toServer.state = Link::Idle;
			toServer.send.live = false;
			toServer.nextAttemptMs = now + 5000 + Random() % 5000;
			++totals.absences;
		}
	}
}

void SimulatedSoak::CheckQuiescedDelivery(int wave)
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		Node &node = nodes[i];
		if (node.rogue)
			continue;
		for (auto &entry : node.links)
		{
			const int remote = entry.first;
			const Link &link = entry.second;
			auto back = nodes[(size_t) remote].links.find((int) i);
			if (link.state != Link::Connected || back == nodes[(size_t) remote].links.end() || back->second.state != Link::Connected)
				continue;

			// Every Reliable message sent on it has arrived, exactly once (duplicates are counted as they arrive).
			if (link.nextReliable > 0)
			{
				const Soak::ExactlyOnceChecker::Stream *stream = reliableChecker.Find(remote, (int) i);
				const uint32_t got = stream && stream->session == link.send.session ? stream->received : 0;
				if (got != link.nextReliable)
					violations.Add("reliable not delivered after quiescing", "wave " + std::to_string(wave) + " " + Name((int) i) + " -> " + Name(remote) +
					                                                             " sent " + std::to_string(link.nextReliable) + " received " + std::to_string(got));
			}
			// Everything sent on a connection both ends still hold has arrived, in order.
			if (link.send.next > 0)
			{
				const Soak::RecvStream *stream = checker.Stream(remote, (int) i);
				if (stream == nullptr || stream->session != link.send.session || stream->expected != link.send.next)
					violations.Add("not delivered after quiescing",
					               "wave " + std::to_string(wave) + " " + Name((int) i) + " -> " + Name(remote) + " sent " +
					                   std::to_string(link.send.next) + " received " + std::to_string(stream && stream->session == link.send.session ? stream->expected : 0) +
					                   "; " + Describe((int) i, remote));
			}
			// Once settled, no datagram on an encrypted connection fails to authenticate: nothing here forges any.
			RakNetStatistics stats;
			if (link.settledDrops < 0)
				violations.Add("connection never settled", Name((int) i) + " -> " + Name(remote));
			else if (node.peer->GetStatistics(nodes[(size_t) remote].address, &stats) != nullptr &&
			         (int64_t) stats.droppedUnauthenticatedDatagrams != link.settledDrops)
				violations.Add("droppedUnauthenticatedDatagrams", Name((int) i) + " from " + Name(remote) + ": " +
				                                                      std::to_string(stats.droppedUnauthenticatedDatagrams - (uint64_t) link.settledDrops) + " after settling");
		}
	}
}

// What a stuck stream looks like from both ends, for the failure message.
std::string SimulatedSoak::Describe(int from, int to)
{
	char text[1024];
	const int client = from == kServer ? to : from;
	const SimulatedNetwork::LinkRule &rule = nodes[(size_t) client].serverLink;
	const Link &link = nodes[(size_t) from].links[to];
	RakNetStatistics s, r;
	const bool hasS = nodes[(size_t) from].peer->GetStatistics(nodes[(size_t) to].address, &s) != nullptr;
	const bool hasR = nodes[(size_t) to].peer->GetStatistics(nodes[(size_t) from].address, &r) != nullptr;
	std::snprintf(text, sizeof(text),
	              "link loss %d%% delay %u jitter %u cap %d; connected %" PRIu64 " ms ago; mtu %d/%d; states %d/%d; "
	              "sender: sendbuf %u/%u/%u/%u resend %u (%" PRIu64 " B) congestion-limited %d (%" PRIu64 " Bps) loss %.2f; receiver: resend %u",
	              rule.lossPercent, rule.delayMs, rule.jitterMs, rule.maxDatagramBytes, NowMs() - link.connectedAtMs,
	              nodes[(size_t) from].peer->GetMTUSize(nodes[(size_t) to].address), nodes[(size_t) to].peer->GetMTUSize(nodes[(size_t) from].address),
	              (int) nodes[(size_t) from].peer->GetConnectionState(nodes[(size_t) to].address), (int) nodes[(size_t) to].peer->GetConnectionState(nodes[(size_t) from].address),
	              hasS ? s.messageInSendBuffer[0] : 0, hasS ? s.messageInSendBuffer[1] : 0, hasS ? s.messageInSendBuffer[2] : 0, hasS ? s.messageInSendBuffer[3] : 0,
	              hasS ? s.messagesInResendBuffer : 0, hasS ? s.bytesInResendBuffer : 0, hasS ? (int) s.isLimitedByCongestionControl : -1,
	              hasS ? s.BPSLimitByCongestionControl : 0, hasS ? s.packetlossTotal : -1.0f, hasR ? r.messagesInResendBuffer : 0);
	return text;
}

void SimulatedSoak::DisconnectEveryone()
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		Node &node = nodes[i];
		for (auto &entry : node.links)
		{
			entry.second.wanted = false;
			if (entry.second.state == Link::Connected)
				node.peer->CloseConnection(nodes[(size_t) entry.first].address, true);
			else if (entry.second.state == Link::Connecting)
				node.peer->CancelConnectionAttempt(nodes[(size_t) entry.first].address);
			// Neither produces a message on this side.
			entry.second.state = Link::Idle;
			entry.second.send.live = false;
		}
	}
}

bool SimulatedSoak::EverythingReleased(std::string *why) const
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		const Node &node = nodes[i];
		if (node.peer->NumberOfConnections() != 0)
		{
			*why = Name((int) i) + " still has " + std::to_string(node.peer->NumberOfConnections()) + " connections";
			return false;
		}
		// No slot in any state (pending, unverified, disconnecting) for any address.
		for (size_t j = 0; j < nodes.size(); ++j)
		{
			if (i == j)
				continue;
			// IS_DISCONNECTED is a closed slot that remembers the address: released.
			const ConnectionState state = node.peer->GetConnectionState(nodes[j].address);
			if (state != IS_NOT_CONNECTED && state != IS_DISCONNECTED)
			{
				*why = Name((int) i) + " still holds " + Name((int) j) + " in state " + std::to_string((int) state);
				return false;
			}
		}
	}
	return true;
}

void SimulatedSoak::Run()
{
	const auto wallStart = std::chrono::steady_clock::now();
	Build();
	const uint64_t startMs = NowMs();
	std::vector<Soak::AllocationSnapshot> afterWave;
	std::vector<int64_t> packetPoolPagesAfterWave;

	for (int wave = 0; wave < waveCount; ++wave)
	{
		SCOPED_TRACE(testing::Message() << "wave " << wave);
		// 1. Everybody (re)connects: to the server, and each cross pair to each other at the same instant.
		for (int i = 1; i <= clientCount; ++i)
		{
			Link &toServer = nodes[(size_t) i].links[kServer];
			toServer.wanted = true;
			toServer.nextAttemptMs = NowMs() + Random() % 2000;
			if (nodes[(size_t) i].partner > 0)
				nodes[(size_t) i].links[nodes[(size_t) i].partner].wanted = true;
		}
		ASSERT_TRUE(RunUntil([&]
		                     { return AllWantedConnected(); }, 120000, true))
		    << "not every client connected; violations so far: " << violations.Total();

		// 2. Sustained traffic.
		RunFor(phaseMs, true);
		// 3. A churn storm, with traffic still flowing, and everyone back.
		CountStepDowns();
		Storm();
		RunFor(phaseMs, true);
		ASSERT_TRUE(RunUntil([&]
		                     { return AllWantedConnected(); }, 120000, true))
		    << "not every client came back after the storm";

		// 4. Quiesce: no new data; everything in flight on a surviving connection arrives.
		RunFor(15000, false);
		CountStepDowns();
		CheckQuiescedDelivery(wave);

		// 5. Everybody leaves; every slot is released.
		DisconnectEveryone();
		std::string why;
		const bool released = RunUntil([&]
		                               { return EverythingReleased(&why); }, 60000, false);
		EXPECT_TRUE(released) << "slot leak: " << why;
		RunFor(1000, false);
		afterWave.push_back(Soak::Allocations());
		int64_t packetPoolPages = 0;
		for (size_t i = 0; i < nodes.size(); ++i)
		{
			// Every inbox is drained each tick, so no packet is out: no page is full, and an idle pool keeps
			// at most DS_MEMORY_POOL_MAX_FREE_PAGES empty ones. This is what bounds the pages left out of the
			// rakMalloc comparison below.
			const DataStructures::MemoryPool<Packet> &pool = PacketPoolInspector::Pool(nodes[i].peer);
			EXPECT_EQ(0, pool.GetUnavailablePagesSize()) << Name((int) i) << ": packet-pool pages still full with nothing queued";
			EXPECT_LE(pool.GetAvailablePagesSize(), DS_MEMORY_POOL_MAX_FREE_PAGES) << Name((int) i) << ": packet pool grew past its free-page bound";
			packetPoolPages += PacketPoolInspector::Pages(nodes[i].peer);
		}
		packetPoolPagesAfterWave.push_back(packetPoolPages);
		std::printf("[soak] wave %d done at %" PRIu64 " s virtual: new blocks %" PRId64 " (%" PRId64 " bytes), rak blocks %" PRId64 " (%" PRId64 " packet-pool pages)\n",
		            wave, (NowMs() - startMs) / 1000, afterWave.back().newBlocks, afterWave.back().newBytes, afterWave.back().rakBlocks, packetPoolPages);
	}

	const double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
	std::printf("[soak] %d clients, %d waves, %" PRIu64 " s virtual in %.1f s wall\n", clientCount, waveCount, (NowMs() - startMs) / 1000, wallSeconds);
	std::printf("[soak] connections %" PRIu64 ", ordered sent %" PRIu64 " delivered %" PRIu64 " (split %" PRIu64 "), reliable sent %" PRIu64
	            " delivered %" PRIu64 ", unreliable sent %" PRIu64 " delivered %" PRIu64 "\n",
	            totals.connectionsCompleted, totals.orderedSent, checker.Delivered(), totals.splitSent, totals.reliableSent, reliableChecker.Delivered(),
	            totals.unreliableSent, totals.unreliableDelivered);
	std::printf("[soak] MTU step-downs %" PRIu64 " (%" PRIu64 " on capped links)\n", totals.mtuStepDowns, totals.mtuStepDownsOnCappedLinks);
	std::printf("[soak] crashes %" PRIu64 ", reconnects %" PRIu64 ", absences %" PRIu64 ", cross-connect rounds %" PRIu64 ", failed attempts %" PRIu64
	            " (%" PRIu64 " after the connection stood), rogue refusals %" PRIu64 ", datagrams sent %" PRIu64 " dropped %" PRIu64 "\n",
	            totals.crashes, totals.gracefulReconnects, totals.absences, totals.crossConnectRounds, totals.failedAttempts, totals.failuresWhileConnected,
	            totals.rogueRefusals, net.DatagramsSent(), net.DatagramsDropped());
	std::printf("[soak] stale datagrams from a previous connection, dropped in the first 2 s of the next: %" PRIu64 " on %" PRIu64 " connections\n",
	            totals.staleDropsExcused, totals.linksWithStaleDrops);

	EXPECT_EQ(0u, violations.Total());
	for (const auto &c : violations.counts)
		ADD_FAILURE() << c.first << ": " << c.second;
	for (const std::string &e : violations.examples)
		std::printf("[soak] violation %s\n", e.c_str());
	EXPECT_EQ(0u, checker.Violations()) << "ordering violations";
	for (const std::string &e : checker.Examples())
		std::printf("[soak] order %s\n", e.c_str());
	EXPECT_EQ(0u, reliableChecker.Violations()) << "Reliable messages delivered twice or corrupted";
	for (const std::string &e : reliableChecker.Examples())
		std::printf("[soak] reliable %s\n", e.c_str());
	EXPECT_GT(totals.rogueRefusals, 0u) << "the plaintext peers were never refused: they did not run";
	EXPECT_GT(totals.crashes, 0u);
	EXPECT_GT(totals.splitSent, 0u);
	EXPECT_GT(totals.unreliableDelivered, 0u);
	EXPECT_GT(reliableChecker.Delivered(), 0u);
	// Nothing here black-holes large datagrams, so every step-down is a false positive of the detector. A
	// rare one can happen by chance; before #94 there were about a third as many as connections.
	EXPECT_LE(totals.mtuStepDowns, totals.connectionsCompleted / 100) << "MTU step-downs on links that do not black-hole";

	// Memory: the first wave warms every pool and container up; after that a wave must not leave more behind.
	if (afterWave.size() >= 3)
	{
		const Soak::AllocationSnapshot &first = afterWave[1];
		const Soak::AllocationSnapshot &last = afterWave.back();
		const int64_t blockSlack = std::max<int64_t>(500, first.newBlocks / 50);
		const int64_t byteSlack = std::max<int64_t>(256 * 1024, first.newBytes / 20);
		if (Soak::CountsOperatorNew())
		{
			EXPECT_LE(last.newBlocks - first.newBlocks, blockSlack) << "operator new blocks grew across waves";
			EXPECT_LE(last.newBytes - first.newBytes, byteSlack) << "operator new bytes grew across waves";
		}
		else
			std::printf("[soak] sanitizer build: operator new is not counted, only rakMalloc\n");
		// Measured noise between waves is one memory-pool page, 3 blocks. The remote-system index leak
		// fixed in ac018381 added about 3 blocks per wave at 200 clients: with it reverted, wave 1 -> last
		// grew by 15 blocks over the default 8 waves and by 24 over the nightly's 12. Three pages of slack
		// keeps both of those failing.
		//
		// Packet-pool pages are left out. A peer's packet pool keeps up to DS_MEMORY_POOL_MAX_FREE_PAGES
		// empty pages of 32 packets for its next burst, so between that bound (checked per peer above) and
		// one page it follows the most packets the peer ever had queued at once, and that moves from wave to
		// wave: a connection whose session reply was lost delivers what was held behind it in one go (#96).
		// A leaked packet still shows: its data is a block of its own.
		const int64_t rakSlack = 9;
		const size_t lastIndex = afterWave.size() - 1;
		const int64_t firstRak = first.rakBlocks - kRakBlocksPerPoolPage * packetPoolPagesAfterWave[1];
		const int64_t lastRak = last.rakBlocks - kRakBlocksPerPoolPage * packetPoolPagesAfterWave[lastIndex];
		EXPECT_LE(lastRak - firstRak, rakSlack) << "rakMalloc blocks (packet-pool pages excluded) grew across waves: " << firstRak << " after wave 1, "
		                                        << lastRak << " after the last";
	}
}
} // namespace

TEST(SimulatedSoak, ManyClientsUnderLossChurnAndCrossConnects)
{
	const uint32_t firstSeed = Soak::EnvUInt("MAFIANET_SOAK_SEED", 1);
	const uint32_t iterations = Soak::EnvUInt("MAFIANET_SOAK_ITERATIONS", 1);
	const int clients = (int) Soak::EnvUInt("MAFIANET_SOAK_CLIENTS", 200);
	const int waves = (int) Soak::EnvUInt("MAFIANET_SOAK_WAVES", 8);
	const uint32_t phaseSeconds = Soak::EnvUInt("MAFIANET_SOAK_PHASE_SECONDS", 30);
	for (uint32_t seed = firstSeed; seed < firstSeed + iterations; ++seed)
	{
		SCOPED_TRACE(testing::Message() << "seed " << seed << " (rerun with MAFIANET_SOAK_SEED=" << seed << ")");
		std::printf("[soak] seed %u\n", seed);
		SimulatedSoak soak(seed, clients, waves, phaseSeconds);
		soak.Run();
		if (HasFatalFailure() || HasNonfatalFailure())
			break;
	}
}
