// Copyright (c) 2026, MafiaHub. MIT license.
#include "SimulatedNetwork.h"
#include <array>
#include <gtest/gtest.h>
#include <thread>

using namespace MafiaNet;

TEST(PlayerRosterThreads, ConcurrentRegistrationsAgreeOnBothDirections)
{
	constexpr uint32_t count = 8;
	SimulatedNetwork net;
	auto *server = net.AddPeer(count, count);
	std::array<PeerGuid, count> guids;
	for (auto &guid : guids)
	{
		auto *client = net.AddPeer(1);
		ASSERT_TRUE(net.Connect(client, server));
		guid = ToPeerGuid(client->GetMyGUID());
	}
	std::array<std::optional<PlayerId>, count> fromWorker{};
	std::thread worker([&]
	                   {
		for (uint32_t i = count; i > 0; --i)
			fromWorker[i - 1] = server->AcceptPlayer(guids[i - 1]); });
	std::array<std::optional<PlayerId>, count> fromCaller{};
	for (uint32_t i = 0; i < count; ++i)
		fromCaller[i] = server->AcceptPlayer(guids[i]);
	worker.join();

	for (uint32_t i = 0; i < count; ++i)
	{
		SCOPED_TRACE(i);
		ASSERT_TRUE(fromCaller[i]);
		EXPECT_EQ(fromCaller[i], fromWorker[i]);
		EXPECT_EQ(server->GetPlayerGuid(*fromCaller[i]), guids[i]);
		EXPECT_EQ(server->GetPlayerId(guids[i]), fromCaller[i]);
	}
}
