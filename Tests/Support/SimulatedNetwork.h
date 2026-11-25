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
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "SimulatedBottleneck.h"
#include "mafianet/get_time.h"
#include "mafianet/peer.h"
#include "mafianet/session_crypto.h"
#include "mafianet/socket2.h"
#include "mafianet/types.h"

namespace MafiaNet
{
enum class JoinPolicy;
class Server;
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
		uint32_t reorderPerThousand = 0;         // chance of an extra delay on a surviving datagram
		uint32_t reorderDelayMs = 0;
		SimulatedBottleneck *bottleneck = nullptr; // caller owns the FIFO for the lifetime of this rule
	};

	// A message a peer's Receive() returned, copied so it outlives the Packet.
	struct Message
	{
		MessageID id;
		std::vector<unsigned char> data;
		SystemAddress systemAddress;
		RakNetGUID guid;
		PlayerId playerId;
	};

	explicit SimulatedNetwork(uint32_t seed = 1);
	~SimulatedNetwork() override;

	SimulatedNetwork(const SimulatedNetwork &) = delete;
	SimulatedNetwork &operator=(const SimulatedNetwork &) = delete;

	Server &AddServer(unsigned short slots, JoinPolicy policy, AdmissionPolicy admission = AdmissionPolicy::Automatic);

	// A started peer bound to 10.0.0.<n>:<port> (port 0 picks one).
	RakPeer *AddPeer(unsigned int maxConnections, unsigned short maxIncoming = 0, unsigned short port = 0);
	// Takes ownership of a peer with a custom transport enqueue implementation.
	RakPeer *AddCustomPeer(std::unique_ptr<RakPeer> peer, unsigned int maxConnections, unsigned short maxIncoming = 0)
	{
		return StartPeer(peer.release(), maxConnections, maxIncoming, 0);
	}
	// A started peer bound to 2001:db8::<n> (RFC 3849 documentation prefix), port 0 picks one.
	// The host counter is shared with AddPeer, so addresses never collide.
	RakPeer *AddPeerV6(unsigned int maxConnections, unsigned short maxIncoming = 0, unsigned short port = 0);
	// A started peer on one dual-stack socket (RakNetSocket2::IsDualStack() is
	// true) bound to both 10.0.0.<n> and 2001:db8::<n>, same <n> and port. It
	// sends to an IPv4 target from the IPv4 address and to an IPv6 target from
	// the IPv6 one. AddressOf(peer) is the IPv6 address.
	RakPeer *AddPeerDualStack(unsigned int maxConnections, unsigned short maxIncoming = 0, unsigned short port = 0);
	// Host name table every simulated socket's Resolve() uses instead of DNS:
	// `name` resolves to `addresses`, in that order (their ports are ignored;
	// Resolve applies the port it is asked for). A numeric literal always
	// resolves to itself; any other unknown name does not resolve.
	void AddHostName(const char *name, std::vector<SystemAddress> addresses);
	// The same, but the peer is constructed in caller-owned storage (at least
	// sizeof(RakPeer) bytes, suitably aligned) that must outlive this network.
	// Lets a test pre-fill that memory to prove the constructor sets what it must.
	RakPeer *AddPeerInStorage(void *storage, unsigned int maxConnections, unsigned short maxIncoming = 0,
	                          unsigned short port = 0);
	SystemAddress AddressOf(const RakPeer *peer) const;
	// The encryption mode every peer added from now on starts with (RakPeer's
	// SetEncryptionMode must be called before Startup). Default: Required.
	void SetNewPeerEncryptionMode(EncryptionMode mode) { newPeerEncryptionMode = mode; }
	// The server identity key (RakPeer::SetIdentityKey, before Startup) every peer
	// added from now on starts with; null (the default) for none.
	void SetNewPeerIdentityKey(const uint8_t *secretKey)
	{
		hasNewPeerIdentityKey = secretKey != nullptr;
		if (hasNewPeerIdentityKey)
			std::memcpy(newPeerIdentityKey, secretKey, sizeof(newPeerIdentityKey));
	}
	// Destroys \a peer as if its process crashed -- no disconnection
	// notification reaches anyone -- and starts a fresh peer (new GUID) bound to
	// the same address. Returns the new peer; \a peer is gone.
	RakPeer *RestartPeer(RakPeer *peer, unsigned int maxConnections, unsigned short maxIncoming = 0);
	// The peer's address of that IP version (4 or 6), or UNASSIGNED_SYSTEM_ADDRESS
	// when it has none: a dual-stack peer has both.
	SystemAddress AddressOf(const RakPeer *peer, int ipVersion) const;

	// Rules apply per direction; unset links use the default rule.
	void SetDefaultLink(const LinkRule &rule) { defaultRule = rule; }
	void SetLink(const RakPeer *from, const RakPeer *to, const LinkRule &rule);
	void SetLinkBothWays(const RakPeer *a, const RakPeer *b, const LinkRule &rule);
	// The same, by address: for one family of a dual-stack peer.
	void SetLink(const SystemAddress &from, const SystemAddress &to, const LinkRule &rule);
	void SetLinkBothWays(const SystemAddress &a, const SystemAddress &b, const LinkRule &rule);

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
	// Start a connection attempt from `from` to `to` without stepping: the
	// raw RakPeer::Connect result, for scenarios that drive the handshake.
	ConnectionAttemptResult StartConnect(RakPeer *from, const RakPeer *to);

	// Deliver raw bytes to a peer as if `from` had sent them, bypassing link
	// rules: for fuzzing what an unconnected remote can send.
	void InjectDatagram(const SystemAddress &from, const RakPeer *to, const unsigned char *data, int length);
	// Called with every datagram any peer sends (before link rules).
	void SetTap(std::function<void(const SystemAddress &from, const SystemAddress &to, const char *data, int length)> callback)
	{
		tap = std::move(callback);
	}

	// Read after Step() on the manual update thread. The two limits can bind together.
	struct SendBudget
	{
		uint32_t windowBytes;
		uint32_t inFlightBytes;
		bool queued;
		uint64_t nakRetryBytes, rtoRetryBytes;
		uint32_t nakRetryWindowCuts, rtoRetryWindowCuts;
		bool capBlocked;
	};
	// Force the same reliability-layer transition as a black hole between manual updates.
	bool StepDownMtu(RakPeer *sender, const SystemAddress &destination);
	// An optional visitor sees only live fragments still waiting for a first send.
	bool ReadSendBudget(const RakPeer *sender, const SystemAddress &destination, SendBudget &budget, const std::function<void(const InternalPacket &)> &visitQueued = {}) const;

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
	enum class Family
	{
		V4,
		V6,
		DualStack
	};
	// hostNumber 0 takes the next free one.
	RakPeer *StartPeer(RakPeer *peer, unsigned int maxConnections, unsigned short maxIncoming, unsigned short port, Family family = Family::V4, unsigned hostNumber = 0);
	int Resolve(const char *host, unsigned short port, SystemAddress *out, int maxOut) const;
	const LinkRule &RuleFor(const SystemAddress &from, const SystemAddress &to) const;
	uint32_t NextRandom();
	static TimeUS VirtualClock();

	std::vector<RakPeer *> peers;
	std::map<RakPeer *, std::unique_ptr<Server>> servers;
	std::set<const RakPeer *> peersInStorage;
	std::map<const RakPeer *, std::vector<Message>> inboxes;
	std::set<const RakPeer *> receiveDisabled;
	std::map<SystemAddress, SimSocket *> sockets; // a dual-stack socket appears under both its addresses
	std::map<const RakPeer *, SystemAddress> ipv4Aliases;
	std::map<std::string, std::vector<SystemAddress>> hostNames;
	SystemAddress pendingIpv4Alias; // for the Create() call StartPeer is about to cause
	std::map<std::pair<SystemAddress, SystemAddress>, LinkRule> links;
	LinkRule defaultRule;
	std::vector<InFlight> inFlight;
	uint64_t nextSequence = 0;
	uint64_t datagramsSent = 0;
	uint64_t datagramsDropped = 0;
	uint32_t lcgState;
	std::function<void(const SystemAddress &, const SystemAddress &, const char *, int)> tap;
	unsigned short nextHost = 1;
	EncryptionMode newPeerEncryptionMode = EncryptionMode::Required;
	bool hasNewPeerIdentityKey = false;
	uint8_t newPeerIdentityKey[64] = {};
};
}
