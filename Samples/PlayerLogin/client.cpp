// Copyright (c) 2026, MafiaHub. MIT license.
#include "protocol.h"
#include "mafianet/dispatcher.h"
#include <chrono>
#include <iostream>
#include <optional>
#include <string_view>

using namespace MafiaNet;
using namespace std::chrono_literals;

int main(int argc, char **argv)
{
	if (argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--hold"))
	{
		std::cerr << "Usage: PlayerLoginClient [ticket] [--hold]\n";
		return 1;
	}
	std::string_view ticket = argc > 1 ? argv[1] : "guest-demo";
	if (ticket.size() > MAXIMUM_SESSION_CONFIG_SIZE)
		return 1;
	Peer client;
	client->SetSessionConfig(ticket.data(), static_cast<unsigned int>(ticket.size()));
	client->SetSessionTimeout(10000);
	SocketDescriptor socket(0, "127.0.0.1");
	if (client->Startup(1, &socket, 1) != RAKNET_STARTED ||
	    client->Connect("127.0.0.1", Login::port, nullptr, 0) != CONNECTION_ATTEMPT_STARTED)
	{
		std::cerr << "Could not start the connection attempt\n";
		return 1;
	}

	Dispatcher messages;
	messages.register_message<Login::WorldLoaded>(Login::worldLoadedId);
	std::optional<std::chrono::steady_clock::time_point> leaveAt;
	messages.on<Login::Welcome>(Login::welcomeId, [&](const Login::Welcome &welcome, const Sender &)
	                            {
		std::cout << "Joined as player " << welcome.playerNumber << std::endl;
		leaveAt = std::chrono::steady_clock::now() + (argc == 3 ? 20s : 0s); });
	auto deadline = std::chrono::steady_clock::now() + 15s;
	for (;;)
	{
		client->WaitForPackets(16);
		while (auto packet = client.receive())
		{
			auto id = packet->data[0];
			switch (id)
			{
			case ID_SESSION_CONFIG_STATUS:
			case ID_CONNECTION_ATTEMPT_FAILED:
			{
				std::string_view detail(reinterpret_cast<const char *>(packet->data + 1), packet->length - 1);
				if (id == ID_CONNECTION_ATTEMPT_FAILED)
				{
					std::cerr << "Connection failed: " << detail << '\n';
					return 1;
				}
				std::cout << "Queue: " << detail << std::endl;
				deadline = std::chrono::steady_clock::now() + 15s;
				break;
			}
			case ID_CONNECTION_REQUEST_ACCEPTED:
				std::cout << "Connected" << std::endl;
				// A real client sends this after its world has finished loading.
				if (!client.send(messages, Login::WorldLoaded{Login::worldRevision}, packet->guid))
					return 1;
				deadline = std::chrono::steady_clock::now() + 15s;
				break;
			case ID_NO_FREE_INCOMING_CONNECTIONS:
			case ID_INCOMPATIBLE_PROTOCOL_VERSION:
			case ID_CONNECTION_LOST:
			case ID_DISCONNECTION_NOTIFICATION:
				std::cerr << "Session ended with message ID " << static_cast<unsigned>(id) << '\n';
				return 1;
			default:
				messages.dispatch(packet);
			}
		}
		auto now = std::chrono::steady_clock::now();
		if (leaveAt && now >= *leaveAt)
		{
			client->Shutdown(300);
			return 0;
		}
		if (!leaveAt && now >= deadline)
		{
			std::cerr << "Timed out waiting for the server\n";
			return 1;
		}
	}
}
