/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// The TCPInterface send buffer (RemoteClient::outgoingData + outgoingOffset): bytes come out in
// order across arbitrary partial sends, and draining a backlog moves O(n) bytes in total, not the
// O(n^2) of erasing the sent prefix after every send. Hermetic: the helpers the update thread uses,
// on a plain vector.

#include <gtest/gtest.h>

#include "mafianet/tcp_interface.h"

#include <cstdint>
#include <deque>
#include <vector>

using namespace MafiaNet;

namespace
{
struct Lcg
{
	uint64_t state;
	uint32_t Below(uint32_t bound)
	{
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return bound == 0 ? 0 : static_cast<uint32_t>(state >> 33) % bound;
	}
};
} // namespace

TEST(TcpOutgoingBuffer, PartialSendsDeliverEveryByteInOrder)
{
	// Differential against a std::deque model: random appends (SendOrBuffer) interleaved with
	// partial sends of random size (the update thread), checking the unsent bytes after each step.
	Lcg rng{42};
	std::vector<unsigned char> buffer;
	std::size_t offset = 0;
	std::deque<unsigned char> model;
	std::vector<unsigned char> sentBytes, modelSent;
	unsigned char next = 0;
	for (int step = 0; step < 20000; ++step)
	{
		if (rng.Below(3) == 0)
		{
			const uint32_t count = rng.Below(3000);
			for (uint32_t i = 0; i < count; ++i, ++next)
			{
				buffer.push_back(next);
				model.push_back(next);
			}
		}
		else
		{
			const std::size_t pending = TcpOutgoingPending(buffer, offset);
			ASSERT_EQ(pending, model.size());
			const std::size_t sent = pending == 0 ? 0 : rng.Below(static_cast<uint32_t>(pending)) + 1;
			sentBytes.insert(sentBytes.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset), buffer.begin() + static_cast<std::ptrdiff_t>(offset + sent));
			for (std::size_t i = 0; i < sent; ++i)
			{
				modelSent.push_back(model.front());
				model.pop_front();
			}
			if (sent != 0)
				TcpOutgoingConsume(buffer, offset, sent);
		}
		// Invariants: the offset stays inside the buffer, and the sent prefix never outgrows the
		// unsent part (an empty buffer has offset 0).
		ASSERT_LE(offset, buffer.size());
		ASSERT_LE(offset, buffer.size() - offset);
		if (buffer.empty())
		{
			ASSERT_EQ(offset, 0u);
		}
		ASSERT_TRUE(std::equal(model.begin(), model.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset)));
	}
	EXPECT_EQ(sentBytes, modelSent);
}

TEST(TcpOutgoingBuffer, DrainingABacklogMovesLinearlyManyBytes)
{
	// 16 MiB queued at once, sent in 4 KiB pieces: 4096 sends. Erasing the sent prefix after each
	// one would move sum(remaining) = about 32 GiB; the offset-based buffer moves less than the
	// backlog itself.
	const std::size_t total = 16u * 1024u * 1024u;
	const std::size_t chunk = 4096;
	std::vector<unsigned char> buffer(total);
	for (std::size_t i = 0; i < total; ++i)
		buffer[i] = static_cast<unsigned char>(i * 31);
	std::size_t offset = 0;
	std::size_t moved = 0;
	std::size_t sent = 0;
	uint64_t eraseEveryTimeMoved = 0;
	while (TcpOutgoingPending(buffer, offset) != 0)
	{
		const std::size_t piece = std::min(chunk, TcpOutgoingPending(buffer, offset));
		ASSERT_EQ(buffer[offset], static_cast<unsigned char>(sent * 31)); // next unsent byte is the right one
		sent += piece;
		eraseEveryTimeMoved += total - sent;
		moved += TcpOutgoingConsume(buffer, offset, piece);
	}
	EXPECT_EQ(sent, total);
	EXPECT_TRUE(buffer.empty());
	EXPECT_LT(moved, total);
	EXPECT_GT(eraseEveryTimeMoved, 1000ull * total); // what the old per-send erase cost
}
