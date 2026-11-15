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

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
// The hash index RakPeer files a remote address under: maxConnections * 8 buckets.
unsigned long Bucket(const SystemAddress &address, unsigned int maxConnections)
{
	return SystemAddress::ToInteger(address) % (maxConnections * 8u);
}
} // namespace

// A remote whose address hashes into a bucket that already holds another remote used to allocate two index
// entries and link only the second: every such connection lost one entry of the pool for the life of the
// peer, so a long-running server grew without bound. Connecting and disconnecting the same colliding client
// over and over must leave the server's index where it was.
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
	const unsigned int before = server->GetLiveRemoteSystemIndexEntriesForTesting();
	const int kVisits = 200;
	for (int i = 0; i < kVisits; ++i)
		visit();
	const unsigned int after = server->GetLiveRemoteSystemIndexEntriesForTesting();

	// The leak lost one index entry per visit.
	EXPECT_EQ(before, after) << "the server kept " << (after - before) << " more index entries after " << kVisits << " visits";
	EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(net.AddressOf(resident)));
	EXPECT_EQ(1u, server->NumberOfConnections());
}
