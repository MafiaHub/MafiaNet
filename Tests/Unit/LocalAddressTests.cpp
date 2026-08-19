/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/types.h"

#include <string>

using namespace MafiaNet;

namespace
{
	const char kUnassignedLabel[] = "UNASSIGNED_SYSTEM_ADDRESS";

	// GetNumberOfAddresses must return exactly the number of leading assigned
	// entries in the local address list: every index below the count is a real
	// address, and the entry at the count (when one exists) is unassigned.
	void ExpectAddressCountMatchesList(RakPeerInterface *peer)
	{
		const unsigned int count = peer->GetNumberOfAddresses();
		ASSERT_GE(count, 1u) << "every host has at least one local address";
		ASSERT_LE(count, (unsigned int) MAXIMUM_NUMBER_OF_INTERNAL_IDS);

		for (unsigned int i = 0; i < count; ++i)
		{
			const std::string ip = peer->GetLocalIP(i);
			EXPECT_FALSE(ip.empty()) << "index " << i;
			EXPECT_NE(ip, kUnassignedLabel)
				<< "GetNumberOfAddresses() returned " << count
				<< " but index " << i << " is unassigned";
		}

		if (count < MAXIMUM_NUMBER_OF_INTERNAL_IDS)
		{
			EXPECT_EQ(std::string(peer->GetLocalIP(count)), kUnassignedLabel)
				<< "count " << count << " is not the exact end of the address list";
		}
	}
}

// An unstarted peer fills the address list on demand.
TEST(LocalAddress, NumberOfAddressesMatchesListBeforeStartup)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	ExpectAddressCountMatchesList(peer);
	RakPeerInterface::DestroyInstance(peer);
}

// A started-but-unconnected peer uses the list filled during Startup.
// Deterministic: no connection, no loopback traffic, no timing.
TEST(LocalAddress, NumberOfAddressesMatchesListAfterStartup)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	SocketDescriptor sd(0, 0);
	ASSERT_EQ(peer->Startup(8, &sd, 1), RAKNET_STARTED);

	ExpectAddressCountMatchesList(peer);

	peer->Shutdown(100);
	RakPeerInterface::DestroyInstance(peer);
}
