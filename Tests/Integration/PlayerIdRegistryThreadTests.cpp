// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/player_ids.h"
#include <array>
#include <gtest/gtest.h>
#include <thread>

using namespace MafiaNet;

TEST(PlayerIdRegistryThreads, ConcurrentRegistrationsAgreeOnBothDirections)
{
	constexpr uint32_t count = 128;
	PlayerIdRegistry players(count);
	std::array<std::optional<PlayerId>, count> fromWorker{};
	std::thread worker([&]
	                   {
		for (uint32_t i = count; i > 0; --i)
			fromWorker[i - 1] = players.Assign(PeerGuid{i - 1}); });
	std::array<std::optional<PlayerId>, count> fromCaller{};
	for (uint32_t i = 0; i < count; ++i)
		fromCaller[i] = players.Assign(PeerGuid{i});
	worker.join();

	for (uint32_t i = 0; i < count; ++i)
	{
		SCOPED_TRACE(i);
		ASSERT_TRUE(fromCaller[i]);
		EXPECT_EQ(fromCaller[i], fromWorker[i]);
		EXPECT_EQ(players.FindPeer(*fromCaller[i]), PeerGuid{i});
		EXPECT_EQ(players.FindId(PeerGuid{i}), fromCaller[i]);
	}
}
