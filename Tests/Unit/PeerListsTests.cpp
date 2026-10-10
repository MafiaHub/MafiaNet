/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/peer.h"
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"

#include <string.h>

using namespace MafiaNet;

// Pins the observable behaviour of RakPeer's protected lists before they move from
// DataStructures::List/Queue to std containers (issue #60):
//
//   banList               -> AddToBanList / RemoveFromBanList / IsBanned / ClearBanList
//   securityExceptionList -> Add/Remove/IsInSecurityExceptionList
//   packetReturnQueue     -> PushBackPacket(packet, pushAtHead) / Receive / GetReceiveBufferSize
//   socketList            -> GetSockets / GetSocket after Startup
//
// All hermetic: no Connect(), no loopback traffic, no wall-clock polling. The one
// place time appears is the temporary-ban expiry, where the ban is created already
// expired rather than waited out.

namespace
{
	class PeerLists : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			peer = RakPeerInterface::GetInstance();
			ASSERT_NE(peer, nullptr);
		}

		void TearDown() override
		{
			if (peer)
			{
				peer->Shutdown(0);
				RakPeerInterface::DestroyInstance(peer);
				peer = 0;
			}
		}

		// Starts the peer on an OS-assigned ephemeral loopback port. No peer ever
		// connects to it; it exists so socketList and the packet pool are populated.
		void StartOnEphemeralPort(unsigned short socketCount = 1)
		{
			SocketDescriptor sd[4];
			ASSERT_LE(socketCount, 4u);
			for (unsigned short i = 0; i < socketCount; i++)
				sd[i] = SocketDescriptor(0, "127.0.0.1");

			ASSERT_EQ(peer->Startup(8, sd, socketCount), RAKNET_STARTED);
		}

		RakPeerInterface *peer;
	};
}

// --- banList ---------------------------------------------------------------

TEST_F(PeerLists, ExactIpIsBannedAndUnbanned)
{
	ASSERT_FALSE(peer->IsBanned("1.2.3.4"));

	peer->AddToBanList("1.2.3.4", 0);
	ASSERT_TRUE(peer->IsBanned("1.2.3.4"));
	ASSERT_FALSE(peer->IsBanned("1.2.3.5")) << "a ban on one address must not cover its neighbour";

	peer->RemoveFromBanList("1.2.3.4");
	ASSERT_FALSE(peer->IsBanned("1.2.3.4"));
}

TEST_F(PeerLists, WildcardBanCoversTheWholePrefix)
{
	peer->AddToBanList("10.0.0.*", 0);

	ASSERT_TRUE(peer->IsBanned("10.0.0.1"));
	ASSERT_TRUE(peer->IsBanned("10.0.0.254"));
	ASSERT_FALSE(peer->IsBanned("10.0.1.1")) << "the wildcard only covers the last octet";
}

// The swap-remove in RemoveFromBanList moves the last entry into the hole. Removing
// a middle entry must leave every other ban queryable: an off-by-one there would
// silently drop the entry that got swapped in.
TEST_F(PeerLists, RemovingAMiddleBanKeepsEveryOtherBan)
{
	const char *ips[] = { "1.1.1.1", "2.2.2.2", "3.3.3.3", "4.4.4.4", "5.5.5.5" };
	for (size_t i = 0; i < 5; i++)
		peer->AddToBanList(ips[i], 0);

	peer->RemoveFromBanList("2.2.2.2");

	ASSERT_FALSE(peer->IsBanned("2.2.2.2"));
	ASSERT_TRUE(peer->IsBanned("1.1.1.1"));
	ASSERT_TRUE(peer->IsBanned("3.3.3.3"));
	ASSERT_TRUE(peer->IsBanned("4.4.4.4"));
	ASSERT_TRUE(peer->IsBanned("5.5.5.5")) << "the entry swapped into the hole must survive";
}

TEST_F(PeerLists, RemovingTheLastBanIsNotASelfAssignmentBug)
{
	peer->AddToBanList("1.1.1.1", 0);
	peer->AddToBanList("2.2.2.2", 0);

	peer->RemoveFromBanList("2.2.2.2"); // index == size-1: swap with itself, then pop

	ASSERT_FALSE(peer->IsBanned("2.2.2.2"));
	ASSERT_TRUE(peer->IsBanned("1.1.1.1"));
}

// AddToBanList must find an existing entry for the IP and update it in place. If it
// appended a second entry instead, one RemoveFromBanList (which removes the first
// match only) would leave the IP banned.
TEST_F(PeerLists, ReAddingAnIpDoesNotCreateASecondEntry)
{
	peer->AddToBanList("7.7.7.7", 0);
	peer->AddToBanList("7.7.7.7", 0);

	ASSERT_TRUE(peer->IsBanned("7.7.7.7"));

	peer->RemoveFromBanList("7.7.7.7");
	ASSERT_FALSE(peer->IsBanned("7.7.7.7"))
		<< "a single RemoveFromBanList must clear the IP, so only one entry was ever stored";
}

// The in-place update also has to be able to *shorten* a ban: a permanent ban
// followed by a 1ms ban for the same IP must expire. An append would leave the
// original permanent entry in the list and the IP would stay banned forever.
TEST_F(PeerLists, ReAddingAnIpWithATimeoutReplacesAPermanentBan)
{
	peer->AddToBanList("8.8.8.8", 0);
	peer->AddToBanList("8.8.8.8", 1);

	RakSleep(30);

	ASSERT_FALSE(peer->IsBanned("8.8.8.8"))
		<< "the 1ms timeout must have replaced the permanent ban on the same entry";
}

// IsBanned sweeps expired entries out as it walks, with a swap-remove that must not
// advance the cursor. Three expired entries in a row is the case that catches a
// wrongly-placed increment.
TEST_F(PeerLists, ExpiredBansAreSweptAndDoNotHideLaterEntries)
{
	peer->AddToBanList("1.1.1.1", 1);
	peer->AddToBanList("2.2.2.2", 1);
	peer->AddToBanList("3.3.3.3", 1);
	peer->AddToBanList("9.9.9.9", 0); // permanent

	RakSleep(30); // the three temporary bans are now in the past

	ASSERT_TRUE(peer->IsBanned("9.9.9.9"))
		<< "the permanent ban must still be found after the expired entries are swept";
	ASSERT_FALSE(peer->IsBanned("1.1.1.1"));
	ASSERT_FALSE(peer->IsBanned("2.2.2.2"));
	ASSERT_FALSE(peer->IsBanned("3.3.3.3"));
}

TEST_F(PeerLists, ClearBanListDropsEverything)
{
	peer->AddToBanList("1.1.1.1", 0);
	peer->AddToBanList("2.2.2.2", 0);
	peer->AddToBanList("3.3.3.*", 0);

	peer->ClearBanList();

	ASSERT_FALSE(peer->IsBanned("1.1.1.1"));
	ASSERT_FALSE(peer->IsBanned("2.2.2.2"));
	ASSERT_FALSE(peer->IsBanned("3.3.3.9"));

	// And the list is usable again afterwards.
	peer->AddToBanList("4.4.4.4", 0);
	ASSERT_TRUE(peer->IsBanned("4.4.4.4"));
}

TEST_F(PeerLists, MalformedBanInputIsIgnored)
{
	peer->AddToBanList(0, 0);
	peer->AddToBanList("", 0);
	peer->AddToBanList("1234567890123456", 0); // 16 chars, over the 15-char limit

	ASSERT_FALSE(peer->IsBanned("1234567890123456"));
	ASSERT_FALSE(peer->IsBanned(0));
	ASSERT_FALSE(peer->IsBanned(""));
}

// --- securityExceptionList -------------------------------------------------

TEST_F(PeerLists, SecurityExceptionMatchesExactlyAndByWildcard)
{
	ASSERT_FALSE(peer->IsInSecurityExceptionList("10.0.0.1"));

	peer->AddToSecurityExceptionList("10.0.0.1");
	peer->AddToSecurityExceptionList("192.168.1.*");

	ASSERT_TRUE(peer->IsInSecurityExceptionList("10.0.0.1"));
	ASSERT_TRUE(peer->IsInSecurityExceptionList("192.168.1.77"));
	ASSERT_FALSE(peer->IsInSecurityExceptionList("192.168.2.77"));
	ASSERT_FALSE(peer->IsInSecurityExceptionList("10.0.0.2"));
}

// RemoveFromSecurityExceptionList removes *every* matching entry, and its swap-remove
// deliberately does not advance the cursor when it removes. Two adjacent duplicates
// at the end of the list is what distinguishes that from a single removal.
TEST_F(PeerLists, RemovingASecurityExceptionRemovesAllMatchingEntries)
{
	peer->AddToSecurityExceptionList("5.5.5.5");
	peer->AddToSecurityExceptionList("10.0.0.1");
	peer->AddToSecurityExceptionList("10.0.0.1");
	peer->AddToSecurityExceptionList("10.0.0.1");

	peer->RemoveFromSecurityExceptionList("10.0.0.1");

	ASSERT_FALSE(peer->IsInSecurityExceptionList("10.0.0.1"))
		<< "all three duplicate entries must be gone";
	ASSERT_TRUE(peer->IsInSecurityExceptionList("5.5.5.5"));
}

TEST_F(PeerLists, RemovingAMiddleSecurityExceptionKeepsTheRest)
{
	peer->AddToSecurityExceptionList("1.1.1.1");
	peer->AddToSecurityExceptionList("2.2.2.2");
	peer->AddToSecurityExceptionList("3.3.3.3");
	peer->AddToSecurityExceptionList("4.4.4.4");

	peer->RemoveFromSecurityExceptionList("2.2.2.2");

	ASSERT_FALSE(peer->IsInSecurityExceptionList("2.2.2.2"));
	ASSERT_TRUE(peer->IsInSecurityExceptionList("1.1.1.1"));
	ASSERT_TRUE(peer->IsInSecurityExceptionList("3.3.3.3"));
	ASSERT_TRUE(peer->IsInSecurityExceptionList("4.4.4.4"));
}

TEST_F(PeerLists, NullIpClearsTheWholeSecurityExceptionList)
{
	peer->AddToSecurityExceptionList("1.1.1.1");
	peer->AddToSecurityExceptionList("2.2.2.2");

	peer->RemoveFromSecurityExceptionList(0);

	ASSERT_FALSE(peer->IsInSecurityExceptionList("1.1.1.1"));
	ASSERT_FALSE(peer->IsInSecurityExceptionList("2.2.2.2"));

	peer->AddToSecurityExceptionList("3.3.3.3");
	ASSERT_TRUE(peer->IsInSecurityExceptionList("3.3.3.3")) << "the list must be reusable after a clear";
}

// --- packetReturnQueue -----------------------------------------------------

namespace
{
	// Allocates a one-byte packet carrying a tag, so Receive order is checkable.
	Packet *TaggedPacket(RakPeerInterface *peer, unsigned char tag)
	{
		Packet *p = peer->AllocatePacket(1);
		p->data[0] = tag;
		return p;
	}
}

TEST_F(PeerLists, PushBackPacketAtTailIsFifo)
{
	StartOnEphemeralPort();

	peer->PushBackPacket(TaggedPacket(peer, 10), false);
	peer->PushBackPacket(TaggedPacket(peer, 20), false);
	peer->PushBackPacket(TaggedPacket(peer, 30), false);

	ASSERT_EQ(peer->GetReceiveBufferSize(), 3u);

	unsigned char seen[3] = { 0, 0, 0 };
	for (int i = 0; i < 3; i++)
	{
		Packet *p = peer->Receive();
		ASSERT_NE(p, nullptr) << "packet " << i << " was pushed but not returned";
		seen[i] = p->data[0];
		peer->DeallocatePacket(p);
	}

	ASSERT_EQ(seen[0], 10);
	ASSERT_EQ(seen[1], 20);
	ASSERT_EQ(seen[2], 30);
}

// pushAtHead is the whole reason the return buffer is a queue and not a stack: a
// plugin that re-pushes a packet it decided not to consume must get it back first.
TEST_F(PeerLists, PushBackPacketAtHeadJumpsTheQueue)
{
	StartOnEphemeralPort();

	peer->PushBackPacket(TaggedPacket(peer, 10), false);
	peer->PushBackPacket(TaggedPacket(peer, 20), false);
	peer->PushBackPacket(TaggedPacket(peer, 99), true);

	unsigned char seen[3] = { 0, 0, 0 };
	for (int i = 0; i < 3; i++)
	{
		Packet *p = peer->Receive();
		ASSERT_NE(p, nullptr);
		seen[i] = p->data[0];
		peer->DeallocatePacket(p);
	}

	ASSERT_EQ(seen[0], 99) << "the head push must be returned before the earlier packets";
	ASSERT_EQ(seen[1], 10);
	ASSERT_EQ(seen[2], 20);
}

TEST_F(PeerLists, InterleavedHeadAndTailPushesKeepBothOrders)
{
	StartOnEphemeralPort();

	peer->PushBackPacket(TaggedPacket(peer, 1), false); // [1]
	peer->PushBackPacket(TaggedPacket(peer, 2), true);  // [2,1]
	peer->PushBackPacket(TaggedPacket(peer, 3), false); // [2,1,3]
	peer->PushBackPacket(TaggedPacket(peer, 4), true);  // [4,2,1,3]

	ASSERT_EQ(peer->GetReceiveBufferSize(), 4u);

	const unsigned char expected[4] = { 4, 2, 1, 3 };
	for (int i = 0; i < 4; i++)
	{
		Packet *p = peer->Receive();
		ASSERT_NE(p, nullptr) << "missing packet at position " << i;
		ASSERT_EQ(p->data[0], expected[i]) << "wrong packet at position " << i;
		peer->DeallocatePacket(p);
	}
}

TEST_F(PeerLists, PushBackPacketIgnoresNull)
{
	StartOnEphemeralPort();

	peer->PushBackPacket(0, false);
	peer->PushBackPacket(0, true);

	ASSERT_EQ(peer->GetReceiveBufferSize(), 0u);
}

// --- socketList ------------------------------------------------------------

TEST_F(PeerLists, GetSocketsReturnsOneSocketPerDescriptorInOrder)
{
	StartOnEphemeralPort(3);

	DataStructures::List<RakNetSocket2 *> sockets;
	peer->GetSockets(sockets);

	ASSERT_EQ(sockets.Size(), 3u) << "one socket per SocketDescriptor passed to Startup";

	// Every socket is distinct and bound to a different port; the indices Startup
	// handed out are the indices GetSockets reports.
	for (unsigned int i = 0; i < sockets.Size(); i++)
	{
		ASSERT_NE(sockets[i], nullptr) << "socket " << i << " is null";
		ASSERT_EQ(sockets[i]->GetUserConnectionSocketIndex(), i)
			<< "socket at index " << i << " does not carry that index";
	}

	for (unsigned int i = 0; i < sockets.Size(); i++)
		for (unsigned int j = i + 1; j < sockets.Size(); j++)
			ASSERT_NE(sockets[i], sockets[j]) << "sockets " << i << " and " << j << " are the same object";
}

TEST_F(PeerLists, GetSocketsIsEmptyBeforeStartup)
{
	DataStructures::List<RakNetSocket2 *> sockets;
	peer->GetSockets(sockets);

	ASSERT_EQ(sockets.Size(), 0u);
}

// GetSockets clears the caller's list up front, so a caller reusing one list across
// calls does not accumulate stale sockets.
TEST_F(PeerLists, GetSocketsClearsTheCallersListFirst)
{
	StartOnEphemeralPort(2);

	DataStructures::List<RakNetSocket2 *> sockets;
	sockets.Push(0, _FILE_AND_LINE_);
	sockets.Push(0, _FILE_AND_LINE_);
	sockets.Push(0, _FILE_AND_LINE_);

	peer->GetSockets(sockets);

	ASSERT_EQ(sockets.Size(), 2u) << "GetSockets must replace the caller's contents, not append to them";
	ASSERT_NE(sockets[0], nullptr);
	ASSERT_NE(sockets[1], nullptr);

	// Calling it twice must be idempotent.
	peer->GetSockets(sockets);
	ASSERT_EQ(sockets.Size(), 2u);
}

// GetSocket(UNASSIGNED_SYSTEM_ADDRESS) runs the same network-thread query but hands
// back only the first socket, so it exercises the copy out of
// SocketQueryOutput::sockets separately from GetSockets.
TEST_F(PeerLists, GetSocketForAnUnassignedAddressReturnsTheFirstSocket)
{
	StartOnEphemeralPort(2);

	DataStructures::List<RakNetSocket2 *> sockets;
	peer->GetSockets(sockets);
	ASSERT_EQ(sockets.Size(), 2u);

	RakNetSocket2 *first = peer->GetSocket(UNASSIGNED_SYSTEM_ADDRESS);
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(first, sockets[0]) << "GetSocket must return the socket GetSockets reports at index 0";
}
