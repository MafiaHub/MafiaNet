// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/client.h"
#include "session_reason.h"

namespace MafiaNet
{
namespace detail
{
class ClientState
{
public:
	explicit ClientState(Peer owner)
	    : peer(std::move(owner)) {}

	enum class Phase
	{
		Connecting,
		Connected,
		Closing,
		Ended
	};
	Peer peer;
	Dispatcher messages;
	Phase phase = Phase::Connecting;
	RakNetGUID server = UNASSIGNED_RAKNET_GUID;
	// What disconnect() sent, reported again when the close completes.
	uint32_t code = 0;
	std::string detail;
	bool polling = false;
	Client::Handlers handlers;

	template <class Event>
	void emit(const typename Event::Argument &event)
	{
		auto &handler = std::get<Client::Handler<Event>>(handlers);
		if (handler)
			handler(event);
	}

	// The connect outcomes RakPeer reports for an outstanding Connect(), other than acceptance.
	static std::optional<ConnectionFailure> failure(MessageID id, unsigned int length)
	{
		switch (id)
		{
		case ID_CONNECTION_ATTEMPT_FAILED:
			// Only a session rejection carries a body; the client-side failures are the bare ID.
			return length > 1 ? ConnectionFailure::Rejected : ConnectionFailure::NoResponse;
		case ID_NO_FREE_INCOMING_CONNECTIONS:
			return ConnectionFailure::ServerFull;
		case ID_CONNECTION_BANNED:
			return ConnectionFailure::Banned;
		case ID_INVALID_PASSWORD:
			return ConnectionFailure::InvalidPassword;
		case ID_INCOMPATIBLE_PROTOCOL_VERSION:
			return ConnectionFailure::IncompatibleProtocol;
		case ID_PUBLIC_KEY_MISMATCH:
			return ConnectionFailure::ServerIdentityMismatch;
		case ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION:
		case ID_OUR_SYSTEM_REQUIRES_SECURITY:
			return ConnectionFailure::EncryptionMismatch;
		case ID_ALREADY_CONNECTED:
			return ConnectionFailure::AlreadyConnected;
		case ID_IP_RECENTLY_CONNECTED:
			return ConnectionFailure::RecentlyConnected;
		default:
			return std::nullopt;
		}
	}

	// True when the packet was a lifecycle event of this session.
	bool handle(const Packet &packet)
	{
		const MessageID id = packet.data[0];
		if (phase == Phase::Connecting)
		{
			if (id == ID_SESSION_CONFIG_STATUS)
			{
				emit<ConnectionStatus>({std::string(reinterpret_cast<const char *>(packet.data + 1), packet.length - 1)});
				return true;
			}
			if (id == ID_CONNECTION_REQUEST_ACCEPTED)
			{
				phase = Phase::Connected;
				server = packet.guid;
				unsigned int length = 0;
				const char *response = peer->GetRemoteSessionConfig(packet.guid, &length);
				emit<ConnectionAccepted>({response ? std::string(response, length) : std::string()});
				return true;
			}
			const auto reason = failure(id, packet.length);
			if (!reason)
				return false;
			phase = Phase::Ended;
			auto sent = *reason == ConnectionFailure::Rejected ? DecodeReason(packet.data + 1, packet.length - 1) : Reason{};
			emit<ConnectionFailed>({*reason, std::move(sent.detail), sent.code});
			return true;
		}
		if ((phase != Phase::Connected && phase != Phase::Closing) || packet.guid != server ||
		    (id != ID_DISCONNECTION_NOTIFICATION && id != ID_CONNECTION_LOST))
			return false;
		ServerDisconnected event{id == ID_CONNECTION_LOST ? LeaveReason::ConnectionLost : LeaveReason::Disconnected, {}, 0};
		if (phase == Phase::Closing)
		{
			event.reason = LeaveReason::Closed;
			event.detail = std::move(detail);
			event.code = code;
		}
		else if (event.reason == LeaveReason::Disconnected)
		{
			auto sent = DecodeReason(packet.data + 1, packet.length - 1);
			event.detail = std::move(sent.detail);
			event.code = sent.code;
			// This peer closed the connection because the server flooded it (InboundRateLimit, FloodAction::Disconnect).
			if (packet.disconnectCause == DisconnectCause::InboundFlood)
				event.reason = LeaveReason::InboundFlood;
		}
		phase = Phase::Ended;
		emit<ServerDisconnected>(event);
		return true;
	}
};
}

using detail::ClientState;

Result<Client> Client::connect(const char *host, unsigned short port, std::string_view session)
{
	auto started = Peer::client().connect(host, port).session(session).start();
	if (!started)
		return Result<Client>::err(started.error());
	return Result<Client>::ok(Client(std::move(*started)));
}

Client::Client(Peer peer)
    : state_(std::make_shared<ClientState>(std::move(peer))) {}
Client::Handlers &Client::handlers()
{
	return state_->handlers;
}
Dispatcher &Client::messages()
{
	return state_->messages;
}
RakPeerInterface &Client::transport()
{
	return *state_->peer.get();
}

bool Client::connected() const
{
	return state_->phase == ClientState::Phase::Connected;
}

std::optional<int> Client::ping() const
{
	if (!connected())
		return std::nullopt;
	const int ping = state_->peer->GetAveragePing(state_->server);
	if (ping < 0)
		return std::nullopt;
	return ping;
}

bool Client::disconnect(uint32_t code, std::string_view text)
{
	if (!connected())
		return false;
	BitStream body;
	const std::string reason = detail::EncodeReason(code, text);
	body.Write(reason.data(), static_cast<unsigned int>(reason.size()));
	state_->peer->CloseConnection(state_->server, true, 0, Priority::Low, reason.empty() ? nullptr : &body);
	state_->phase = ClientState::Phase::Closing;
	state_->code = code;
	state_->detail = std::string(text);
	return true;
}

uint32_t Client::send_encoded(const BitStream &bits, Reliability reliability, Priority priority, char channel)
{
	if (!connected())
		return 0;
	return state_->peer->Send(&bits, priority, reliability, channel, state_->server, false);
}

void Client::poll()
{
	const auto state = state_;
	if (state->polling)
		return;
	state->polling = true;
	struct Reset
	{
		bool &flag;
		~Reset() { flag = false; }
	} reset{state->polling};
	while (auto packet = state->peer.receive())
	{
		if (packet->length && state->handle(*packet))
			continue;
		state->messages.dispatch(packet);
	}
}
}
