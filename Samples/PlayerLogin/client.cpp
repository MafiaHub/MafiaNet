// Copyright (c) 2026, MafiaHub. MIT license.
#include "protocol.h"
#include "mafianet/client.h"
#include <chrono>
#include <iostream>
#include <optional>
#include <string_view>

using namespace MafiaNet;
using namespace std::chrono_literals;

namespace
{
std::string_view describe(const ConnectionFailed &failed)
{
	switch (failed.reason)
	{
	case ConnectionFailure::Rejected:
		return failed.detail;
	case ConnectionFailure::ServerFull:
		return "The server is not taking new players";
	case ConnectionFailure::NoResponse:
		return "No answer from the server";
	default:
		return "The connection was refused";
	}
}
}

int main(int argc, char **argv)
{
	if (argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--hold"))
	{
		std::cerr << "Usage: PlayerLoginClient [ticket] [--hold]\n";
		return 1;
	}
	const bool hold = argc == 3;
	auto client = Client::connect("127.0.0.1", Login::port, argc > 1 ? argv[1] : "guest-demo");
	if (!client)
	{
		std::cerr << "Could not start the connection attempt\n";
		return 1;
	}
	client->transport().SetSessionTimeout(10000);

	std::optional<int> exitCode;
	std::optional<std::chrono::steady_clock::time_point> leaveAt;
	auto deadline = std::chrono::steady_clock::now() + 15s;
	client->on<ConnectionStatus>([&](const ConnectionStatus &status)
	                             {
		std::cout << "Queue: " << status.update << std::endl;
		deadline = std::chrono::steady_clock::now() + 15s; });
	client->on<ConnectionAccepted>([&](const ConnectionAccepted &)
	                               {
		std::cout << "Connected" << std::endl;
		// A real client sends this after its world has finished loading.
		client->send(Login::WorldLoaded{Login::worldRevision});
		deadline = std::chrono::steady_clock::now() + 15s; });
	client->on<ConnectionFailed>([&](const ConnectionFailed &failed)
	                             {
		std::cerr << "Connection failed: " << describe(failed) << '\n';
		exitCode = 1; });
	client->on<ServerDisconnected>([&](const ServerDisconnected &event)
	                               {
		if (event.reason != LeaveReason::Closed)
			std::cerr << "Session ended: " << (event.detail.empty() ? "connection lost" : event.detail) << '\n';
		exitCode = event.reason == LeaveReason::Closed ? 0 : 1; });
	client->messages().register_message<Login::WorldLoaded>(Login::worldLoadedId);
	client->messages().on<Login::Welcome>(Login::welcomeId, [&](const Login::Welcome &welcome, const Sender &)
	                                      {
		std::cout << "Joined as player " << welcome.playerNumber << std::endl;
		leaveAt = std::chrono::steady_clock::now() + (hold ? 20s : 0s); });

	while (!exitCode)
	{
		client->transport().WaitForPackets(16);
		client->poll();
		auto now = std::chrono::steady_clock::now();
		if (leaveAt && now >= *leaveAt)
		{
			// The server reads the code and detail from PlayerLeft. Poll until the close completes.
			client->disconnect(Login::finished, "Finished");
			leaveAt.reset();
			deadline = now + 5s;
		}
		else if (!leaveAt && now >= deadline)
		{
			std::cerr << "Timed out waiting for the server\n";
			return 1;
		}
	}
	return *exitCode;
}
