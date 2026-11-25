// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include "mafianet/peer.h"
#include "player_roster.h"
#include <deque>

namespace MafiaNet
{
namespace detail
{
class ServerState
{
public:
	explicit ServerState(Peer owner)
	    : peer(std::move(owner)) {}
	RakPeer &transport() const { return *static_cast<RakPeer *>(peer.get()); }

	std::optional<PlayerId> accept(const ClientConnection &connection)
	{
		auto key = connection.key();
		auto it = clients.find(key.guid);
		if (it == clients.end() || it->second.generation != key.generation || it->second.closing)
			return std::nullopt;
		auto id = transport().playerRoster->Accept(key.guid, key.generation);
		if (id && !it->second.joined)
		{
			joins.push_back(Player(connection, *id));
			it->second.joined = true;
		}
		return id;
	}

	std::optional<PlayerRoster::ConnectionInfo> current(ConnectionKey key) const
	{
		return transport().playerRoster->FindConnection(key.guid, key.generation);
	}

	uint32_t send(ConnectionKey key, const char *bytes, BitSize_t bits, Reliability reliability, Priority priority, char channel, bool close)
	{
		auto it = clients.find(key.guid);
		if (it == clients.end() || it->second.generation != key.generation || it->second.closing)
			return 0;
		if (static_cast<unsigned char>(channel) >= NUMBER_OF_ORDERED_STREAMS || !current(key) ||
		    static_cast<unsigned>(priority) >= NUMBER_OF_PRIORITIES || static_cast<unsigned>(reliability) >= NUMBER_OF_RELIABILITIES)
			return 0;
		using Slot = RakPeer::RemoteSystemStruct;
		const auto receipt = transport().IncrementNextSendReceipt();
		if (!transport().SendBuffered(bytes, bits, priority, reliability, channel, ToGuid(key.guid), false,
		                              close ? Slot::DISCONNECT_ASAP : Slot::NO_ACTION, receipt, true, key.generation))
			return 0;
		it->second.closing = close;
		return receipt;
	}

	enum class Decision
	{
		Accept,
		Reject,
		Status
	};
	bool answer(ConnectionKey key, Decision decision, std::string_view data)
	{
		if (data.size() > MAXIMUM_SESSION_CONFIG_SIZE)
			return false;
		using Command = RakPeer::BufferedCommandStruct;
		auto command = Command::BCS_SESSION_ACCEPT;
		if (decision == Decision::Reject)
			command = Command::BCS_SESSION_REJECT;
		else if (decision == Decision::Status)
			command = Command::BCS_SESSION_STATUS;
		return transport().QueueSessionCommand(ToGuid(key.guid), command, data.data(), static_cast<unsigned int>(data.size()), key.generation);
	}

	std::optional<Player> find_player(const std::shared_ptr<ServerState> &owner, PlayerId id) const
	{
		auto guid = transport().GetPlayerGuid(id);
		if (!guid)
			return std::nullopt;
		auto it = clients.find(*guid);
		if (it == clients.end() || !it->second.joined)
			return std::nullopt;
		ConnectionKey key{*guid, it->second.generation};
		auto info = current(key);
		if (!info || info->id != id)
			return std::nullopt;
		return Player(ClientConnection(owner, key), id);
	}

	std::vector<Player> players(const std::shared_ptr<ServerState> &owner) const
	{
		std::vector<Player> result;
		transport().playerRoster->VisitPlayers([&](PlayerId id, PeerGuid guid, uint64_t generation)
		                                       {
			const auto it = clients.find(guid);
			if (it != clients.end() && it->second.joined && it->second.generation == generation)
				result.push_back(Player(ClientConnection(owner, {guid, generation}), id)); });
		return result;
	}

	struct Client
	{
		uint64_t generation;
		bool joined = false;
		bool closing = false;
	};
	Peer peer;
	Dispatcher messages;
	std::unordered_map<PeerGuid, Client> clients;
	std::deque<Player> joins;
	bool polling = false;
	Server::Handlers handlers;
	template <class Event>
	void emit(const typename Event::Argument &event)
	{
		auto &handler = std::get<Server::Handler<Event>>(handlers);
		if (handler)
			handler(event);
	}
};
}

using detail::ServerState;

bool ConnectionRequest::accept(std::string_view data) const
{
	auto state = state_.lock();
	return state && state->answer(key_, ServerState::Decision::Accept, data);
}

bool ConnectionRequest::reject(std::string_view reason) const
{
	auto state = state_.lock();
	return state && state->answer(key_, ServerState::Decision::Reject, reason);
}

bool ConnectionRequest::status(std::string_view update) const
{
	auto state = state_.lock();
	return state && state->answer(key_, ServerState::Decision::Status, update);
}

std::optional<PlayerId> ClientConnection::accept_player() const
{
	auto state = state_.lock();
	return state ? state->accept(*this) : std::nullopt;
}

std::optional<int> ClientConnection::ping() const
{
	auto state = state_.lock();
	auto info = state ? state->current(key_) : std::nullopt;
	if (info && info->ping >= 0)
		return info->ping;
	return std::nullopt;
}

bool ClientConnection::disconnect() const
{
	auto state = state_.lock();
	const char notification = ID_DISCONNECTION_NOTIFICATION;
	return state && state->send(key_, &notification, 8, Reliability::ReliableOrdered, Priority::High, 0, true);
}

Dispatcher &ClientConnection::messages(ServerState &state)
{
	return state.messages;
}

uint32_t ClientConnection::send_encoded(ServerState &state, const BitStream &bits, Reliability reliability, Priority priority, char channel) const
{
	return state.send(key_, reinterpret_cast<const char *>(bits.GetData()), bits.GetNumberOfBitsUsed(), reliability, priority, channel, false);
}

Result<Server> Server::listen(unsigned short port, unsigned short max_clients)
{
	auto started = Peer::server().port(port).max_connections(max_clients).start();
	if (!started)
		return Result<Server>::err(started.error());
	return Result<Server>::ok(Server(std::move(*started)));
}

Server::Server(Peer peer)
    : state_(std::make_shared<ServerState>(std::move(peer))) {}
Server::Handlers &Server::handlers()
{
	return state_->handlers;
}
Dispatcher &Server::messages()
{
	return state_->messages;
}
RakPeerInterface &Server::transport()
{
	return state_->transport();
}
std::optional<Player> Server::player(PlayerId id) const
{
	return state_->find_player(state_, id);
}

std::vector<Player> Server::players() const
{
	return state_->players(state_);
}

std::optional<ClientConnection> Server::connection(const Sender &sender) const
{
	ConnectionKey key{sender.peer_guid(), sender.packet().connectionGeneration};
	if (!key.generation || !state_->current(key))
		return std::nullopt;
	return ClientConnection(state_, key);
}

void Server::poll()
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
	for (;;)
	{
		while (!state->joins.empty())
		{
			Player joined = state->joins.front();
			state->joins.pop_front();
			state->emit<PlayerJoined>(joined);
		}
		auto packet = state->peer.receive();
		if (!packet)
			break;
		const ConnectionKey key{ToPeerGuid(packet->guid), packet->connectionGeneration};
		if (key.generation && packet->length)
		{
			switch (packet->data[0])
			{
			case ID_SESSION_CONFIG_REQUEST:
				state->emit<ConnectionRequested>(ConnectionRequest(state, key, std::string(reinterpret_cast<const char *>(packet->data + 1), packet->length - 1)));
				continue;
			case ID_SESSION_CONFIG_ABANDONED:
				state->emit<ConnectionAbandoned>({key});
				continue;
			case ID_NEW_INCOMING_CONNECTION:
			{
				state->clients.insert_or_assign(key.guid, ServerState::Client{key.generation});
				ClientConnection connection(state, key);
				state->emit<ClientConnected>(connection);
				continue;
			}
			case ID_CONNECTION_LOST:
			case ID_DISCONNECTION_NOTIFICATION:
			{
				auto it = state->clients.find(key.guid);
				if (it == state->clients.end() || it->second.generation != key.generation)
					continue;
				state->clients.erase(it);
				const auto reason = packet->data[0] == ID_CONNECTION_LOST ? LeaveReason::ConnectionLost : LeaveReason::Disconnected;
				const std::string detail(reinterpret_cast<const char *>(packet->data + 1), packet->length - 1);
				if (packet->playerId != PlayerId{})
					state->emit<PlayerLeft>({key, packet->playerId, reason, detail});
				state->emit<ClientDisconnected>({key, reason, detail});
				continue;
			}
			}
		}
		state->messages.dispatch(packet);
	}
}
}
