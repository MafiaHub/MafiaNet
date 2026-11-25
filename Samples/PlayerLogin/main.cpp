// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include <iostream>

using namespace MafiaNet;

// Both ends register this message at ID_USER_PACKET_ENUM. A client sends it after
// loading the indicated world revision. Account authentication belongs to the game.
struct WorldLoaded
{
	uint32_t revision = 0;
	template <class Archive>
	void serialize(Archive &archive) { archive & revision; }
};

int main()
{
	auto server = Server::listen(60000, {32});
	if (!server)
		return 1;
	const auto check_world = [](const ConnectionRequest &request)
	{
		if (request.payload() != "world-v1")
		{
			request.reject("Unsupported world version");
			return;
		}
		if (request.accept() == SessionAcceptResult::Full)
			request.status("Waiting for a slot");
	};
	server->on<ConnectionRequested>(check_world);
	// Only validated requests remain here. A disconnect or an abandoned claim can release capacity.
	const auto retry_waiting = [&](const auto &)
	{
		for (const auto &request : server->pending_requests())
			if (request.accept() == SessionAcceptResult::Full)
				break;
	};
	server->on<ClientDisconnected>(retry_waiting);
	server->on<ConnectionAbandoned>(retry_waiting);
	server->on<ClientConnected>([](ClientConnection connection)
	                            { std::cout << "Waiting for client " << static_cast<uint64_t>(connection.key().guid) << " to finish loading\n"; });
	server->messages().on<WorldLoaded>(ID_USER_PACKET_ENUM, [&](const WorldLoaded &message, const Sender &sender)
	                                   {
		if (auto connection = server->connection(sender))
		{
			if (message.revision == 1)
				connection->accept_player();
			else
				connection->disconnect();
		} });
	server->on<PlayerJoined>([](Player player)
	                         { std::cout << "Player " << static_cast<uint32_t>(player.id()) << " entered the world\n"; });
	for (;;)
	{
		server->transport().WaitForPackets(16);
		server->poll();
	}
}
