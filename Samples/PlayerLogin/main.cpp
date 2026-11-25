// Copyright (c) 2026, MafiaHub. MIT license.
#include "protocol.h"
#include "mafianet/server.h"
#include <chrono>
#include <iostream>

using namespace MafiaNet;
using namespace std::chrono_literals;

namespace
{
// Fixed tickets for this loopback example. A game would verify an account ticket here.
void admit(const ConnectionRequest &request)
{
	ConnectionSlot slot;
	if (request.payload() == "guest-demo")
		slot = ConnectionSlot::Public;
	else if (request.payload() == "staff-demo")
		slot = ConnectionSlot::Reserved;
	else
	{
		request.reject("Unknown ticket");
		return;
	}
	switch (request.accept(slot))
	{
	case SessionAcceptResult::Full:
		request.status("Waiting for a slot");
		break;
	case SessionAcceptResult::InvalidPayload:
	case SessionAcceptResult::OutOfMemory:
		request.reject("Login unavailable");
		break;
	case SessionAcceptResult::Accepted:
	case SessionAcceptResult::Expired:
		break;
	}
}
}

int main()
{
	ServerCapacity capacity;
	capacity.connected = 2;
	capacity.reserved = 1;
	capacity.pending = 8;
	capacity.pending_per_address = 4;
	auto peer = Peer::server().host("127.0.0.1").port(Login::port).capacity(capacity).start();
	if (!peer)
	{
		std::cerr << "Could not listen on port " << Login::port << '\n';
		return 1;
	}
	Server server(std::move(*peer));
	server.on<ConnectionRequested>(admit);
	server.on<ClientConnected>([](const ClientConnection &)
	                           { std::cout << "Connected, waiting for WorldLoaded\n"; });
	server.messages().on<Login::WorldLoaded>(Login::worldLoadedId, [&](const Login::WorldLoaded &message, const Sender &sender)
	                                         {
		if (auto connection = server.connection(sender))
		{
			if (message.revision == Login::worldRevision)
				connection->accept_player();
			else
				connection->disconnect();
		} });
	server.messages().register_message<Login::Welcome>(Login::welcomeId);
	server.on<PlayerJoined>([](Player player)
	                        {
		auto number = static_cast<uint32_t>(player.id());
		std::cout << "Player " << number << " joined" << std::endl;
		player.send(Login::Welcome{number}); });
	server.on<PlayerLeft>([](const PlayerLeft &left)
	                      { std::cout << "Player " << static_cast<uint32_t>(left.player_id) << " left" << std::endl; });

	std::cout << "Listening on 127.0.0.1:" << Login::port << std::endl;
	auto nextQueueUpdate = std::chrono::steady_clock::now() + 1s;
	for (;;)
	{
		server.transport().WaitForPackets(16);
		server.poll();
		auto now = std::chrono::steady_clock::now();
		if (now >= nextQueueUpdate)
		{
			for (const auto &request : server.pending_requests())
				admit(request);
			nextQueueUpdate = now + 1s;
		}
	}
}
