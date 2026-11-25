// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/server.h"
#include <iostream>

using namespace MafiaNet;

struct Welcome
{
	uint32_t number;
	template <class Archive>
	void serialize(Archive &archive) { archive & number; }
};

int main()
{
	auto server = Server::listen(60000, 32);
	if (!server)
		return 1;
	server->messages().register_message<Welcome>(ID_USER_PACKET_ENUM);
	server->on<PlayerJoined>([](Player player)
	                         {
		const auto number = static_cast<uint32_t>(player.id());
		std::cout << "Player " << number << " joined\n";
		player.send(Welcome{number}); });
	server->on<PlayerLeft>([](const PlayerLeft &event)
	                       { std::cout << "Player " << static_cast<uint32_t>(event.player_id) << " left\n"; });
	for (;;)
	{
		server->transport().WaitForPackets(16);
		server->poll();
	}
}
