/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RakPeer's address -> remote system hash index (ReferenceRemoteSystem /
 *  DereferenceRemoteSystem), run on the SimulatedNetwork.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>

#include "SimulatedNetwork.h"

#include "mafianet/memory_override.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
// Live blocks allocated through rakMalloc_Ex / rakRealloc_Ex while the hooks are installed. Memory pools
// allocate their pages there, so a pool that keeps growing shows up here.
std::atomic<long> g_liveRakBlocks{0};

void *CountingMalloc(size_t size, const char *, unsigned int)
{
	void *p = std::malloc(size);
	if (p != nullptr)
		++g_liveRakBlocks;
	return p;
}

void *CountingRealloc(void *p, size_t size, const char *, unsigned int)
{
	void *q = std::realloc(p, size);
	if (p == nullptr && q != nullptr)
		++g_liveRakBlocks;
	return q;
}

void CountingFree(void *p, const char *, unsigned int)
{
	if (p != nullptr)
		--g_liveRakBlocks;
	std::free(p);
}

// Installs the counting hooks for the lifetime of the object; every block they count is released through
// them too, so the count stays meaningful whatever the test allocated before.
class RakAllocationCounter
{
public:
	RakAllocationCounter()
	    : oldMalloc(GetMalloc_Ex()), oldRealloc(GetRealloc_Ex()), oldFree(GetFree_Ex())
	{
		SetMalloc_Ex(CountingMalloc);
		SetRealloc_Ex(CountingRealloc);
		SetFree_Ex(CountingFree);
	}
	~RakAllocationCounter()
	{
		SetMalloc_Ex(oldMalloc);
		SetRealloc_Ex(oldRealloc);
		SetFree_Ex(oldFree);
	}
	long Live() const { return g_liveRakBlocks.load(); }

private:
	void *(*oldMalloc)(size_t, const char *, unsigned int);
	void *(*oldRealloc)(void *, size_t, const char *, unsigned int);
	void (*oldFree)(void *, const char *, unsigned int);
};

// The hash index RakPeer files a remote address under: maxConnections * 8 buckets.
unsigned long Bucket(const SystemAddress &address, unsigned int maxConnections)
{
	return SystemAddress::ToInteger(address) % (maxConnections * 8u);
}
} // namespace

// A remote whose address hashes into a bucket that already holds another remote used to allocate two index
// entries and link only the second: every such connection lost one entry of the pool for the life of the
// peer, so a long-running server grew without bound. Connecting and disconnecting the same colliding client
// over and over must leave the server's memory where it was.
TEST(RemoteSystemLookup, CollidingAddressesDoNotLeakIndexEntries)
{
	const unsigned int kServerSlots = 2;
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(kServerSlots, kServerSlots);
	RakPeer *resident = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(resident, server));

	// A second client on a port whose address lands in the resident's bucket.
	const unsigned long residentBucket = Bucket(net.AddressOf(resident), kServerSlots);
	unsigned short port = 0;
	for (unsigned short candidate = 2000; candidate < 60000 && port == 0; ++candidate)
	{
		SystemAddress probe("10.0.0.3", candidate);
		if (Bucket(probe, kServerSlots) == residentBucket)
			port = candidate;
	}
	ASSERT_NE(0, port);
	RakPeer *visitor = net.AddPeer(1, 0, port);
	ASSERT_EQ(SystemAddress("10.0.0.3", port), net.AddressOf(visitor));
	const SystemAddress serverAddress = net.AddressOf(server);
	const SystemAddress visitorAddress = net.AddressOf(visitor);

	RakAllocationCounter counter;
	auto visit = [&]
	{
		net.Inbox(visitor).clear();
		net.Inbox(server).clear();
		ASSERT_TRUE(net.Connect(visitor, server));
		visitor->CloseConnection(serverAddress, true);
		ASSERT_TRUE(net.RunUntil([&]
		                         { return server->GetConnectionState(visitorAddress) != IS_CONNECTED && server->NumberOfConnections() == 1 &&
			                              visitor->NumberOfConnections() == 0; }, 5000));
		net.Step(100);
	};

	// Warm up every pool the visit touches, then measure.
	for (int i = 0; i < 10; ++i)
		visit();
	const long before = counter.Live();
	const int kVisits = 200;
	for (int i = 0; i < kVisits; ++i)
		visit();
	const long grown = counter.Live() - before;

	// The leak lost one 32-entry-page slot per visit: 200 visits took six or more pages (three blocks each).
	EXPECT_LT(grown, 6) << "the server kept " << grown << " more blocks after " << kVisits << " visits";
	EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(net.AddressOf(resident)));
	EXPECT_EQ(1u, server->NumberOfConnections());
}
