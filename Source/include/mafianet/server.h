// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/dispatcher.h"
#include <memory>
#include <string_view>
#include <tuple>

namespace MafiaNet
{
namespace detail
{
class ServerState;
}

enum class LeaveReason
{
	Disconnected,
	ConnectionLost
};

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
	ConnectionKey key() const { return key_; }
	const std::string &payload() const { return payload_; }
	bool accept(std::string_view response = {}) const;
	bool reject(std::string_view reason = {}) const;
	bool status(std::string_view update) const;

private:
	friend class Server;
	ConnectionRequest(std::weak_ptr<detail::ServerState> state, ConnectionKey key, std::string payload)
	    : state_(std::move(state)), key_(key), payload_(std::move(payload)) {}
	std::weak_ptr<detail::ServerState> state_;
	ConnectionKey key_;
	std::string payload_;
};

/// A connected client can exchange login messages before the game accepts a player.
class RAK_DLL_EXPORT ClientConnection
{
public:
	ConnectionKey key() const { return key_; }
	std::optional<PlayerId> accept_player() const;
	bool disconnect() const;
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
	bool disconnect() const { return connection_.disconnect(); }
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
struct ClientDisconnected
{
	using Argument = ClientDisconnected;
	ConnectionKey connection;
	LeaveReason reason;
	std::string detail;
};
struct PlayerLeft
{
	using Argument = PlayerLeft;
	ConnectionKey connection;
	PlayerId player_id;
	LeaveReason reason;
	std::string detail;
};

/// Owns a listening Peer. Configure handlers before polling. Poll and use handles on the game thread.
/// Worker threads should post their results back to that thread with the original handle.
class RAK_DLL_EXPORT Server
{
public:
	static Result<Server> listen(unsigned short port, unsigned short max_clients);
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
	Dispatcher &messages();
	RakPeerInterface &transport();
	/// Recover the exact connection that sent a dispatched message, including a pre-join login.
	std::optional<ClientConnection> connection(const Sender &sender) const;
	std::optional<Player> player(PlayerId id) const;
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
