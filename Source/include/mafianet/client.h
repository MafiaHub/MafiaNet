// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/dispatcher.h"
#include "mafianet/session_admission.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace MafiaNet
{
namespace detail
{
class ClientState;
}

/// Why a connection attempt ended before the server accepted the session.
enum class ConnectionFailure
{
	Rejected,               ///< The server application refused the session; code and detail are its reason.
	NoResponse,             ///< Nothing answered: unreachable, timed out, or refused by a raw peer without a reason.
	ServerFull,             ///< ID_NO_FREE_INCOMING_CONNECTIONS: the server is not taking new handshakes.
	Banned,                 ///< ID_CONNECTION_BANNED
	InvalidPassword,        ///< ID_INVALID_PASSWORD
	IncompatibleProtocol,   ///< ID_INCOMPATIBLE_PROTOCOL_VERSION
	ServerIdentityMismatch, ///< ID_PUBLIC_KEY_MISMATCH: the server failed the pinned identity check.
	EncryptionMismatch,     ///< ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION or ID_OUR_SYSTEM_REQUIRES_SECURITY
	AlreadyConnected,       ///< ID_ALREADY_CONNECTED
	RecentlyConnected       ///< ID_IP_RECENTLY_CONNECTED
};

// Local callback types, separate from the application's wire message table.
/// The server accepted the session. \a response is what it passed to ConnectionRequest::accept().
struct ConnectionAccepted
{
	using Argument = ConnectionAccepted;
	std::string response;
};
/// The server is still deciding and sent ConnectionRequest::status().
struct ConnectionStatus
{
	using Argument = ConnectionStatus;
	std::string update;
};
/// The attempt ended without a connection. A rejection carries the server's detail and code.
struct ConnectionFailed
{
	using Argument = ConnectionFailed;
	ConnectionFailure reason;
	std::string detail;
	uint32_t code = 0;
};
/// The established session ended. detail and code are what the closing end sent.
struct ServerDisconnected
{
	using Argument = ServerDisconnected;
	LeaveReason reason;
	std::string detail;
	uint32_t code = 0;
};

/// Owns a connecting Peer for one session with a game server. Configure handlers before polling.
/// Poll and call the client on the game thread. Every attempt ends with exactly one ConnectionFailed
/// or ConnectionAccepted, and an accepted session with exactly one ServerDisconnected.
class RAK_DLL_EXPORT Client
{
public:
	/// Start a client peer and begin connecting. The server reads \a session from ConnectionRequest::payload().
	static Result<Client> connect(const char *host, unsigned short port, std::string_view session = {});
	/// Take ownership of a peer from Peer::client().connect(...).start() with custom settings, such as
	/// pin_server_identity(), session() or a password, before receiving its first packet.
	explicit Client(Peer peer);
	Client(Client &&) noexcept = default;
	Client &operator=(Client &&) noexcept = default;
	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	template <class Event>
	void on(std::function<void(const typename Event::Argument &)> handler)
	{
		std::get<std::function<void(const typename Event::Argument &)>>(handlers()) = std::move(handler);
	}
	/// Drain lifecycle events and typed messages on the calling thread.
	void poll();
	Dispatcher &messages();
	RakPeerInterface &transport();
	/// True from ConnectionAccepted until disconnect() or ServerDisconnected.
	bool connected() const;
	std::optional<int> ping() const;
	/// Close the session gracefully. The server reads \a code (application-defined, zero means none) and
	/// \a detail; ServerDisconnected then reports LeaveReason::Closed with the same values.
	bool disconnect(uint32_t code = 0, std::string_view detail = {});

	/// Send a message registered with messages() to the server. Returns the receipt, or zero.
	template <class T>
	uint32_t send(const T &message, Reliability reliability = Reliability::ReliableOrdered, Priority priority = Priority::High, char channel = 0)
	{
		if (!messages().has<T>())
			return 0;
		BitStream bits;
		messages().encode(bits, message);
		return send_encoded(bits, reliability, priority, channel);
	}

private:
	friend class detail::ClientState;
	template <class Event>
	using Handler = std::function<void(const typename Event::Argument &)>;
	using Handlers = std::tuple<Handler<ConnectionStatus>, Handler<ConnectionAccepted>, Handler<ConnectionFailed>, Handler<ServerDisconnected>>;
	Handlers &handlers();
	uint32_t send_encoded(const BitStream &bits, Reliability reliability, Priority priority, char channel);
	std::shared_ptr<detail::ClientState> state_;
};
}
