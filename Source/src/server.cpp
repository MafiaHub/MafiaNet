// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include "mafianet/peer.h"
#include "player_roster.h"
#include "session_reason.h"
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

	bool close(ConnectionKey key, uint32_t code, std::string_view detail)
	{
		std::string notification(1, static_cast<char>(ID_DISCONNECTION_NOTIFICATION));
		notification += EncodeReason(code, detail);
		if (!send(key, notification.data(), BYTES_TO_BITS(static_cast<BitSize_t>(notification.size())), Reliability::ReliableOrdered, Priority::High, 0, true))
			return false;
		auto &client = clients.at(key.guid);
		client.code = code;
		client.detail = std::string(detail);
		return true;
	}

	SessionAcceptResult answer(ConnectionKey key, SessionDecision decision, std::string_view data, ConnectionSlot slot = ConnectionSlot::Public)
	{
		if (data.size() > MAXIMUM_SESSION_CONFIG_SIZE)
			return SessionAcceptResult::InvalidPayload;
		return transport().QueueSessionCommand(ToGuid(key.guid), decision, data.data(), static_cast<unsigned int>(data.size()), key.generation, slot);
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

	std::vector<PendingSessionPtr> pending_requests() const { return transport().playerRoster->PendingRequests(); }
	void publish(const PendingSessionPtr &request) { transport().playerRoster->Publish(request); }

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
		uint64_t generation = 0;
		bool joined = false;
		bool closing = false;
		// What disconnect() sent, reported again when the close completes.
		uint32_t code = 0;
		std::string detail;
	};
	Peer peer;
	// Packets taken from the transport but over a poll budget, per connection in arrival order. Declared after
	// `peer` so they are freed before it. A connection stays in parkedOrder while it has any.
	std::unordered_map<PeerGuid, std::deque<PacketPtr>> parked;
	std::deque<PeerGuid> parkedOrder;
	std::size_t parkedCount = 0;
	PollStats lastPoll;
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

ConnectionKey ConnectionRequest::key() const
{
	return {request_->guid, request_->generation};
}
const SystemAddress &ConnectionRequest::address() const
{
	return request_->address;
}
const std::string &ConnectionRequest::payload() const
{
	return request_->payload;
}

SessionAcceptResult ConnectionRequest::accept(ConnectionSlot slot, std::string_view data) const
{
	auto state = state_.lock();
	return state ? state->answer(key(), detail::SessionDecision::Accept, data, slot) : SessionAcceptResult::Expired;
}

bool ConnectionRequest::reject(std::string_view text) const
{
	return reject(0, text);
}

bool ConnectionRequest::reject(uint32_t code, std::string_view text) const
{
	auto state = state_.lock();
	// Framed even when empty, so the client can tell a refusal from an attempt nobody answered.
	return state && state->answer(key(), detail::SessionDecision::Reject, detail::EncodeReason(code, text, text.empty())) == SessionAcceptResult::Accepted;
}

bool ConnectionRequest::status(std::string_view update) const
{
	auto state = state_.lock();
	return state && state->answer(key(), detail::SessionDecision::Status, update) == SessionAcceptResult::Accepted;
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

bool ClientConnection::disconnect(uint32_t code, std::string_view detail) const
{
	auto state = state_.lock();
	return state && state->close(key_, code, detail);
}

Dispatcher &ClientConnection::messages(ServerState &state)
{
	return state.messages;
}

uint32_t ClientConnection::send_encoded(ServerState &state, const BitStream &bits, Reliability reliability, Priority priority, char channel) const
{
	return state.send(key_, reinterpret_cast<const char *>(bits.GetData()), bits.GetNumberOfBitsUsed(), reliability, priority, channel, false);
}

Result<Server> Server::listen(unsigned short port, ServerCapacity capacity, InboundRateLimit inbound)
{
	auto started = Peer::server().port(port).capacity(capacity).inbound_rate_limit(std::move(inbound)).start();
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

std::vector<ConnectionRequest> Server::pending_requests() const
{
	std::vector<ConnectionRequest> result;
	auto pending = state_->pending_requests();
	result.reserve(pending.size());
	for (auto &request : pending)
		result.push_back(ConnectionRequest(state_, std::move(request)));
	return result;
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

namespace
{
// How many packets a budgeted poll may park, as a multiple of maxPackets.
constexpr uint64_t kParkFactor = 8;

// The packets Server::poll turns into lifecycle events instead of dispatching to message handlers.
bool IsLifecycle(const Packet &packet)
{
	if (!packet.connectionGeneration || !packet.length)
		return false;
	switch (packet.data[0])
	{
	case ID_SESSION_CONFIG_REQUEST:
	case ID_SESSION_CONFIG_ABANDONED:
	case ID_NEW_INCOMING_CONNECTION:
	case ID_CONNECTION_LOST:
	case ID_DISCONNECTION_NOTIFICATION:
		return true;
	default:
		return false;
	}
}
}

void Server::poll()
{
	poll(PollBudget{});
}

std::size_t Server::backlog() const
{
	return state_->parkedCount + state_->transport().GetReceiveBufferSize();
}

PollStats Server::last_poll() const
{
	return state_->lastPoll;
}

void Server::poll(PollBudget budget)
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

	constexpr uint64_t kUnbounded = ~uint64_t{0};
	const uint64_t limit = budget.maxPackets ? budget.maxPackets : kUnbounded;
	const uint64_t perConnection = budget.maxPacketsPerConnection ? budget.maxPacketsPerConnection : kUnbounded;
	uint64_t handled = 0;
	std::unordered_map<PeerGuid, uint64_t> handledPerConnection;
	// Takes one application packet from the budget if both the call and the connection have room.
	auto take = [&](PeerGuid guid)
	{
		if (handled >= limit)
			return false;
		if (perConnection != kUnbounded)
		{
			auto &own = handledPerConnection[guid];
			if (own >= perConnection)
				return false;
			++own;
		}
		++handled;
		return true;
	};
	auto flush_joins = [&]
	{
		while (!state->joins.empty())
		{
			Player joined = state->joins.front();
			state->joins.pop_front();
			state->emit<PlayerJoined>(joined);
		}
	};
	auto process = [&](const PacketPtr &packet)
	{
		const ConnectionKey key{ToPeerGuid(packet->guid), packet->connectionGeneration};
		if (IsLifecycle(*packet))
		{
			switch (packet->data[0])
			{
			case ID_SESSION_CONFIG_REQUEST:
			{
				auto request = std::make_shared<detail::PendingSession>(detail::PendingSession{key.guid, key.generation, packet->systemAddress, std::string(reinterpret_cast<const char *>(packet->data + 1), packet->length - 1)});
				state->publish(request);
				state->emit<ConnectionRequested>(ConnectionRequest(state, std::move(request)));
				return;
			}
			case ID_SESSION_CONFIG_ABANDONED:
				state->emit<ConnectionAbandoned>({key});
				return;
			case ID_NEW_INCOMING_CONNECTION:
			{
				ServerState::Client client;
				client.generation = key.generation;
				state->clients.insert_or_assign(key.guid, std::move(client));
				ClientConnection connection(state, key);
				state->emit<ClientConnected>(connection);
				return;
			}
			default: // ID_CONNECTION_LOST, ID_DISCONNECTION_NOTIFICATION
			{
				auto it = state->clients.find(key.guid);
				if (it == state->clients.end() || it->second.generation != key.generation)
					return;
				auto reason = packet->data[0] == ID_CONNECTION_LOST                      ? LeaveReason::ConnectionLost
				              : packet->disconnectCause == DisconnectCause::InboundFlood ? LeaveReason::InboundFlood
				                                                                         : LeaveReason::Disconnected;
				auto sent = detail::DecodeReason(packet->data + 1, packet->length - 1);
				if (it->second.closing)
				{
					reason = LeaveReason::Closed;
					sent = {it->second.code, std::move(it->second.detail)};
				}
				state->clients.erase(it);
				if (packet->playerId != PlayerId{})
					state->emit<PlayerLeft>({key, packet->playerId, reason, sent.detail, sent.code});
				state->emit<ClientDisconnected>({key, reason, std::move(sent.detail), sent.code});
				return;
			}
			}
		}
		state->messages.dispatch(packet);
	};
	auto park = [&](PeerGuid guid, PacketPtr packet)
	{
		auto [it, added] = state->parked.try_emplace(guid);
		if (added)
			state->parkedOrder.push_back(guid);
		it->second.push_back(std::move(packet));
		++state->parkedCount;
	};

	// Parked packets are older than anything still in the transport: serve them first, one connection after
	// another, each up to its own share. A connection that still has some goes to the back of the line, so a
	// large backlog rotates among connections instead of the first one always winning.
	for (std::size_t turns = state->parkedOrder.size(); turns > 0; --turns)
	{
		const PeerGuid guid = state->parkedOrder.front();
		state->parkedOrder.pop_front();
		auto &queue = state->parked.at(guid);
		while (!queue.empty())
		{
			flush_joins();
			// A lifecycle event costs nothing but stays behind this connection's earlier packets.
			if (!IsLifecycle(*queue.front()) && !take(guid))
				break;
			process(queue.front());
			queue.pop_front();
			--state->parkedCount;
		}
		if (queue.empty())
			state->parked.erase(guid);
		else
			state->parkedOrder.push_back(guid);
	}

	// Then new packets, until the budget is spent. Over-share packets are parked rather than left in the
	// transport's single queue, which cannot skip one connection. Parking is cheap (a move) but still capped, at
	// kParkFactor times the budget, so the call stays bounded when one connection floods; a flooder ahead of
	// others in the queue is looked past up to that many packets.
	uint64_t parkedNow = 0;
	const uint64_t parkLimit = limit == kUnbounded ? kUnbounded : limit * kParkFactor;
	while (handled < limit && parkedNow < parkLimit)
	{
		flush_joins();
		auto packet = state->peer.receive();
		if (!packet)
			break;
		const PeerGuid guid = ToPeerGuid(packet->guid);
		if (state->parked.count(guid))
		{
			// Behind this connection's parked packets, to keep its order.
			park(guid, std::move(packet));
			++parkedNow;
		}
		else if (IsLifecycle(*packet) || take(guid))
			process(packet);
		else
		{
			park(guid, std::move(packet));
			++parkedNow;
		}
	}
	flush_joins();
	state->lastPoll = {static_cast<uint32_t>(handled), state->parkedCount + state->transport().GetReceiveBufferSize()};
}
}
