// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/dispatcher.h"
#include "mafianet/session_admission.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>

namespace MafiaNet
{
namespace detail
{
class ServerState;
struct PendingSession;
}

/// Identifies one local incarnation of a remote connection. Safe to retain across polls.
struct ConnectionKey
{
	PeerGuid guid;
	uint64_t generation;
	friend bool operator==(ConnectionKey a, ConnectionKey b) { return a.guid == b.guid && a.generation == b.generation; }
	friend bool operator!=(ConnectionKey a, ConnectionKey b) { return !(a == b); }
};

class RAK_DLL_EXPORT ConnectionRequest
{
public:
	ConnectionKey key() const;
	const std::string &payload() const;
	const SystemAddress &address() const;
	SessionAcceptResult accept(ConnectionSlot slot = ConnectionSlot::Public, std::string_view response = {}) const;
	/// The client reports ConnectionFailed with ConnectionFailure::Rejected and this detail.
	bool reject(std::string_view detail = {}) const;
	/// Also carry an application-defined reason code (zero means none) to the client.
	bool reject(uint32_t code, std::string_view detail = {}) const;
	bool status(std::string_view update) const;

private:
	friend class Server;
	ConnectionRequest(std::weak_ptr<detail::ServerState> state, std::shared_ptr<const detail::PendingSession> request)
	    : state_(std::move(state)), request_(std::move(request)) {}
	std::weak_ptr<detail::ServerState> state_;
	std::shared_ptr<const detail::PendingSession> request_;
};

/// A connected client can exchange login messages before the game accepts a player.
class RAK_DLL_EXPORT ClientConnection
{
public:
	ConnectionKey key() const { return key_; }
	std::optional<PlayerId> accept_player() const;
	/// Close gracefully. The remote reads \a code (application-defined, zero means none) and \a detail,
	/// and this server reports LeaveReason::Closed with the same values.
	bool disconnect(uint32_t code = 0, std::string_view detail = {}) const;
	std::optional<int> ping() const;

	template <class T>
	uint32_t send(const T &message, Reliability reliability = Reliability::ReliableOrdered, Priority priority = Priority::High, char channel = 0) const
	{
		auto state = state_.lock();
		if (!state || !messages(*state).has<T>())
			return 0;
		BitStream bits;
		messages(*state).encode(bits, message);
		return send_encoded(*state, bits, reliability, priority, channel);
	}

private:
	friend class Server;
	friend class detail::ServerState;
	ClientConnection(std::weak_ptr<detail::ServerState> state, ConnectionKey key)
	    : state_(std::move(state)), key_(key) {}
	static Dispatcher &messages(detail::ServerState &state);
	uint32_t send_encoded(detail::ServerState &state, const BitStream &bits, Reliability reliability, Priority priority, char channel) const;
	std::weak_ptr<detail::ServerState> state_;
	ConnectionKey key_;
};

/// A copyable handle to one accepted join. Operations on an expired session fail.
class Player
{
public:
	PlayerId id() const { return id_; }
	ConnectionKey key() const { return connection_.key(); }
	bool disconnect(uint32_t code = 0, std::string_view detail = {}) const { return connection_.disconnect(code, detail); }
	std::optional<int> ping() const { return connection_.ping(); }
	template <class T>
	uint32_t send(const T &message, Reliability reliability = Reliability::ReliableOrdered, Priority priority = Priority::High, char channel = 0) const
	{
		return connection_.send(message, reliability, priority, channel);
	}

private:
	friend class detail::ServerState;
	Player(ClientConnection connection, PlayerId id)
	    : connection_(std::move(connection)), id_(id) {}
	ClientConnection connection_;
	PlayerId id_;
};

// Local callback types are separate from the application's wire message table.
struct ConnectionRequested
{
	using Argument = ConnectionRequest;
};
struct ClientConnected
{
	using Argument = ClientConnection;
};
struct PlayerJoined
{
	using Argument = Player;
};
struct ConnectionAbandoned
{
	using Argument = ConnectionAbandoned;
	ConnectionKey connection;
};
/// detail and code are what the closing end sent (empty and zero for a lost connection).
struct ClientDisconnected
{
	using Argument = ClientDisconnected;
	ConnectionKey connection;
	LeaveReason reason;
	std::string detail;
	uint32_t code = 0;
};
struct PlayerLeft
{
	using Argument = PlayerLeft;
	ConnectionKey connection;
	PlayerId player_id;
	LeaveReason reason;
	std::string detail;
	uint32_t code = 0;
};

/// Bounds the work one Server::poll() call does, so a tick ends even while packets keep arriving.
/// Zero means unbounded for either field; a default-constructed budget behaves like the unbudgeted poll().
/// Packets over the budget are not dropped: they wait, in arrival order per connection, for a later poll.
struct PollBudget
{
	/// Most application packets (typed messages and unknown ids) handled by one call. Connection
	/// lifecycle events do not count against it.
	uint32_t maxPackets = 0;
	/// Most application packets handled for one connection by one call, so a flooding connection leaves
	/// room for the others. Packets over it are parked behind that connection's earlier ones.
	uint32_t maxPacketsPerConnection = 0;
};

/// What the last completed poll() did.
struct PollStats
{
	/// Application packets handled (lifecycle events excluded).
	uint32_t handled = 0;
	/// Packets still waiting after the call: parked by the budget plus those not yet taken from the transport.
	std::size_t backlog = 0;
};

/// Owns a listening Peer. Configure handlers before polling. Poll and use handles on the game thread.
/// Worker threads should post their results back to that thread with the original handle.
class RAK_DLL_EXPORT Server
{
public:
	/// \a inbound limits the messages each client may send (see RakPeerInterface::SetInboundRateLimit).
	static Result<Server> listen(unsigned short port, ServerCapacity capacity, InboundRateLimit inbound = {});
	/// Take ownership of a peer started with a custom ServerBuilder configuration.
	explicit Server(Peer peer);
	Server(Server &&) noexcept = default;
	Server &operator=(Server &&) noexcept = default;
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	template <class Event>
	void on(std::function<void(const typename Event::Argument &)> handler)
	{
		std::get<std::function<void(const typename Event::Argument &)>>(handlers()) = std::move(handler);
	}
	/// Drain lifecycle events and typed messages on the calling thread.
	void poll();
	/// Like poll(), but handles at most \a budget packets. Call it again to continue; packets stay in order per
	/// connection. Lifecycle events of a connection are never reordered with its own packets, and events of a
	/// connection with no parked packets are handled in the call that reaches them: parked packets of other
	/// connections never delay them, and the call stops taking packets from the transport only once
	/// maxPackets were handled or as many were parked, so an event waits behind at most about 9 * maxPackets packets of other connections.
	void poll(PollBudget budget);
	/// Counters of the last completed poll.
	PollStats last_poll() const;
	/// Packets waiting now: parked by a budget plus queued in the transport (datagrams the network thread has not
	/// yet decoded are not counted).
	std::size_t backlog() const;
	Dispatcher &messages();
	RakPeerInterface &transport();
	/// Recover the exact connection that sent a dispatched message, including a pre-join login.
	std::optional<ClientConnection> connection(const Sender &sender) const;
	std::optional<Player> player(PlayerId id) const;
	/// Unanswered requests in callback order. Full requests stay here until answered or abandoned.
	std::vector<ConnectionRequest> pending_requests() const;
	/// Snapshot of accepted players in ascending number order.
	std::vector<Player> players() const;

private:
	friend class detail::ServerState;
	template <class Event>
	using Handler = std::function<void(const typename Event::Argument &)>;
	using Handlers = std::tuple<Handler<ConnectionRequested>, Handler<ConnectionAbandoned>, Handler<ClientConnected>, Handler<ClientDisconnected>, Handler<PlayerJoined>, Handler<PlayerLeft>>;
	Handlers &handlers();
	std::shared_ptr<detail::ServerState> state_;
};
}
