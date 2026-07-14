/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/guid_util.h"
#include "mafianet/types.h"
#include "mafianet/peerinterface.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace MafiaNet;

namespace
{
	const uint64_t kSamples[] = { 0u, 1u, 42u, 0x123456789ABCDEF0ull, (uint64_t) -2 };
}

TEST(GuidUtil, ToStringMatchesLegacyMember)
{
	for (uint64_t sample : kSamples)
	{
		const RakNetGUID g(sample);
		char buffer[64];
		g.ToString(buffer, sizeof(buffer));
		EXPECT_EQ(to_string(g), std::string(buffer)) << "guid value 0x" << std::hex << sample;
	}
}

TEST(GuidUtil, UnassignedSentinelStringifiesToDocumentedLabel)
{
	EXPECT_EQ(to_string(UNASSIGNED_RAKNET_GUID), std::string("UNASSIGNED_RAKNET_GUID"));
}

// The legacy member used a process-wide rotating static buffer, so two
// concurrent callers could read each other's half-written output. to_string
// owns a local buffer, so every result must equal its own expected text no
// matter how many threads run. A shared buffer would corrupt these.
TEST(GuidUtil, ToStringIsThreadSafe)
{
	const unsigned threadCount = 16;
	const unsigned iterations = 20000;
	std::atomic<bool> corrupted(false);

	std::vector<std::thread> threads;
	threads.reserve(threadCount);
	for (unsigned t = 0; t < threadCount; ++t)
	{
		// Each thread owns a distinct value, so its expected string is fixed.
		const uint64_t value = 0x1000000000000000ull + t;
		const RakNetGUID g(value);
		const std::string expected = to_string(g);
		threads.emplace_back([g, expected, iterations, &corrupted]() {
			for (unsigned k = 0; k < iterations && !corrupted.load(std::memory_order_relaxed); ++k)
			{
				if (to_string(g) != expected)
					corrupted.store(true, std::memory_order_relaxed);
			}
		});
	}
	for (std::thread &th : threads)
		th.join();

	EXPECT_FALSE(corrupted.load()) << "to_string produced a corrupted result under concurrency";
}

// connected_address needs a started peer, but never opens a connection:
// deterministic, no loopback traffic, no timing.
class GuidUtilConnectedAddressTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		peer = RakPeerInterface::GetInstance();
		SocketDescriptor sd(0, 0);
		ASSERT_EQ(peer->Startup(8, &sd, 1), RAKNET_STARTED);
	}

	void TearDown() override
	{
		peer->Shutdown(100);
		RakPeerInterface::DestroyInstance(peer);
	}

	RakPeerInterface *peer = nullptr;
};

TEST_F(GuidUtilConnectedAddressTest, SentinelAndUnknownGuidsMapToNullopt)
{
	// GetSystemAddressFromGuid(UNASSIGNED_RAKNET_GUID) returns the sentinel.
	EXPECT_FALSE(connected_address(*peer, UNASSIGNED_RAKNET_GUID).has_value());

	// An arbitrary guid we are not connected to also has no address.
	EXPECT_FALSE(connected_address(*peer, RakNetGUID(0xDEADBEEFull)).has_value());
}

TEST_F(GuidUtilConnectedAddressTest, MirrorsLegacyLookup)
{
	// has_value() iff the legacy call did not return the sentinel, and when
	// engaged the address is exactly what the legacy call returned.
	for (uint64_t sample : kSamples)
	{
		const RakNetGUID g(sample);
		const SystemAddress legacy = peer->GetSystemAddressFromGuid(g);
		const std::optional<SystemAddress> modern = connected_address(*peer, g);
		const bool legacyHas = (legacy != UNASSIGNED_SYSTEM_ADDRESS);
		ASSERT_EQ(modern.has_value(), legacyHas) << "guid value 0x" << std::hex << sample;
		if (legacyHas)
			EXPECT_EQ(modern.value(), legacy) << "guid value 0x" << std::hex << sample;
	}
}
