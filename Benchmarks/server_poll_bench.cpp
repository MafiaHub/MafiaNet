// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/client.h"
#include "mafianet/server.h"
#include "SimulatedNetwork.h"
#include <benchmark/benchmark.h>
#include <vector>

using namespace MafiaNet;

namespace
{
struct Ping
{
	uint32_t value = 0;
	template <class Archive>
	void serialize(Archive &ar) { ar & value; }
};
constexpr MessageID kPingId = ID_USER_PACKET_ENUM;
constexpr unsigned kClients = 100;
constexpr unsigned kPerClient = 300;

// A server with kClients connected players whose messages pile up in its transport queue while it does not poll.
struct Backlog
{
	SimulatedNetwork net;
	Server &server;
	std::vector<Client *> clients;
	uint64_t handled = 0;

	Backlog()
	    : server(net.AddServer({kClients}))
	{
		server.on<ConnectionRequested>([](const ConnectionRequest &request)
		                               { request.accept(); });
		server.on<ClientConnected>([](ClientConnection connection)
		                           { connection.accept_player(); });
		server.messages().on<Ping>(kPingId, [this](const Ping &, const Sender &)
		                           { ++handled; });
		auto *host = static_cast<RakPeer *>(&server.transport());
		for (unsigned i = 0; i < kClients; ++i)
		{
			clients.push_back(&net.AddClient(host));
			clients.back()->messages().register_message<Ping>(kPingId);
		}
		net.RunUntil([&]
		             {
			server.poll();
			bool all = true;
			for (auto *client : clients)
			{
				client->poll();
				all = all && client->connected();
			}
			return all; }, 20000);
	}

	// Refill until the transport holds every message, without the server polling.
	void Fill()
	{
		for (auto *client : clients)
			for (unsigned i = 0; i < kPerClient; ++i)
				client->send(Ping{i});
		net.RunUntil([&]
		             { return server.backlog() >= kClients * kPerClient; }, 60000);
	}
};

// One poll over a full backlog. Unbudgeted this is the loop that kept the game tick busy for as long as
// packets arrived; with a budget it handles a fixed slice and returns.
void PollBacklog(benchmark::State &state, PollBudget budget)
{
	Backlog rig;
	for (auto _ : state)
	{
		state.PauseTiming();
		while (rig.server.backlog())
			rig.server.poll();
		rig.Fill();
		state.ResumeTiming();
		rig.server.poll(budget);
	}
	state.counters["handled_per_poll"] = static_cast<double>(rig.server.last_poll().handled);
	state.counters["backlog_left"] = static_cast<double>(rig.server.backlog());
}
}

static void BM_ServerPoll_Unbudgeted(benchmark::State &state)
{
	PollBacklog(state, PollBudget{});
}
BENCHMARK(BM_ServerPoll_Unbudgeted)->Unit(benchmark::kMicrosecond)->Iterations(5);

static void BM_ServerPoll_Budget1000(benchmark::State &state)
{
	PollBacklog(state, PollBudget{1000, 30});
}
BENCHMARK(BM_ServerPoll_Budget1000)->Unit(benchmark::kMicrosecond)->Iterations(5);

static void BM_ServerPoll_Budget100(benchmark::State &state)
{
	PollBacklog(state, PollBudget{100, 3});
}
BENCHMARK(BM_ServerPoll_Budget100)->Unit(benchmark::kMicrosecond)->Iterations(5);
