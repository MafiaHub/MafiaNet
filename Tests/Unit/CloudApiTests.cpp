/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// CloudClient / CloudServer through their std::vector APIs, on the simulated network: Post, Get
// (all systems and specific systems), Release, Unsubscribe, query filters and a Get answered
// across two servers.

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/cloud_client.h"
#include "mafianet/cloud_server.h"
#include "mafianet/message_identifiers.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

// CloudServer owns raw-pointer trees; as an exported class its copy must be explicitly deleted.
static_assert(!std::is_copy_constructible_v<CloudServer>, "CloudServer must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<CloudServer>, "CloudServer must not be copy-assignable");

namespace
{
// Functions, not globals: a namespace-scope RakString would be built during static
// initialization, possibly before RakString's own allocator in another translation unit.
CloudKey Scores()
{
	return CloudKey("App", 1);
}
CloudKey Names()
{
	return CloudKey("App", 2);
}

// Records what each filter hook saw; can drop a key from a release or reject a Get.
class RecordingFilter : public CloudServerQueryFilter
{
public:
	bool OnPostRequest(RakNetGUID, SystemAddress, CloudKey, uint32_t, const char *) override { return true; }
	bool OnReleaseRequest(RakNetGUID, SystemAddress, std::vector<CloudKey> &cloudKeys) override
	{
		releaseKeyCounts.push_back(cloudKeys.size());
		if (keepFirstReleasedKeyOnly && cloudKeys.size() > 1)
			cloudKeys.resize(1);
		return true;
	}
	bool OnGetRequest(RakNetGUID, SystemAddress, CloudQuery &query, std::vector<RakNetGUID> &specificSystems) override
	{
		getKeyCounts.push_back(query.keys.size());
		getSpecificCounts.push_back(specificSystems.size());
		return allowGets;
	}
	bool OnUnsubscribeRequest(RakNetGUID, SystemAddress, std::vector<CloudKey> &cloudKeys, std::vector<RakNetGUID> &specificSystems) override
	{
		unsubscribeCounts.push_back(cloudKeys.size() * 100 + specificSystems.size());
		return true;
	}

	bool keepFirstReleasedKeyOnly = false;
	bool allowGets = true;
	std::vector<size_t> releaseKeyCounts, getKeyCounts, getSpecificCounts, unsubscribeCounts;
};

Packet AsPacket(SimulatedNetwork::Message &message)
{
	Packet packet{};
	packet.data = message.data.data();
	packet.length = static_cast<unsigned int>(message.data.size());
	packet.bitSize = BYTES_TO_BITS(packet.length);
	packet.systemAddress = message.systemAddress;
	packet.guid = message.guid;
	return packet;
}

std::string RowData(const CloudQueryRow *row)
{
	return std::string(reinterpret_cast<const char *>(row->data), row->length);
}

class CloudApi : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server = net.AddPeer(8, 8);
		ASSERT_NE(server, nullptr);
		server->AttachPlugin(&cloudServer);
		for (int i = 0; i < 2; i++)
		{
			clients[i] = net.AddPeer(1);
			ASSERT_NE(clients[i], nullptr);
			clients[i]->AttachPlugin(&cloudClients[i]);
			ASSERT_TRUE(net.Connect(clients[i], server)) << "client " << i;
		}
	}

	void TearDown() override
	{
		for (int i = 0; i < 2; i++)
			if (clients[i])
				clients[i]->DetachPlugin(&cloudClients[i]);
		if (server)
			server->DetachPlugin(&cloudServer);
	}

	void Post(int client, const CloudKey &key, const std::string &value)
	{
		CloudKey copy = key;
		cloudClients[client].Post(&copy, reinterpret_cast<const unsigned char *>(value.data()), static_cast<uint32_t>(value.size()), server->GetMyGUID());
	}

	// Steps the network long enough for every queued reliable message to be handled.
	void Settle()
	{
		net.RunUntil([]
		             { return false; }, 300);
	}

	// Waits for the next ID_CLOUD_GET_RESPONSE of \a client and decodes it into \a result.
	bool NextGetResponse(int client, CloudQueryResult &result)
	{
		if (!net.RunUntilReceived(clients[client], ID_CLOUD_GET_RESPONSE, 2000))
			return false;
		std::vector<SimulatedNetwork::Message> &inbox = net.Inbox(clients[client]);
		const auto it = std::find_if(inbox.begin(), inbox.end(), [](const SimulatedNetwork::Message &m)
		                             { return m.id == ID_CLOUD_GET_RESPONSE; });
		SimulatedNetwork::Message message = *it;
		inbox.erase(it);
		Packet packet = AsPacket(message);
		cloudClients[client].OnGetReponse(&result, &packet);
		return true;
	}

	bool ReceivedNotification(int client)
	{
		std::vector<SimulatedNetwork::Message> &inbox = net.Inbox(clients[client]);
		const auto it = std::find_if(inbox.begin(), inbox.end(), [](const SimulatedNetwork::Message &m)
		                             { return m.id == ID_CLOUD_SUBSCRIPTION_NOTIFICATION; });
		if (it == inbox.end())
			return false;
		inbox.erase(it);
		return true;
	}

	// Declared before the network, so they outlive the peers it destroys.
	CloudServer cloudServer;
	CloudClient cloudClients[2];
	SimulatedNetwork net;
	RakPeer *server = nullptr;
	RakPeer *clients[2] = {nullptr, nullptr};
};
} // namespace

TEST_F(CloudApi, GetFillsRowsReturnedVector)
{
	Post(0, Scores(), "alpha");
	Post(1, Scores(), "bravo");
	Post(0, Names(), "zulu");
	Settle();

	CloudQuery query;
	query.keys = {Scores(), Names()};
	ASSERT_TRUE(cloudClients[0].Get(&query, server->GetMyGUID()));

	CloudQueryResult result;
	ASSERT_TRUE(NextGetResponse(0, result));
	ASSERT_EQ(result.rowsReturned.size(), 3u);
	EXPECT_EQ(result.cloudQuery.keys.size(), 2u);
	EXPECT_TRUE(result.resultKeyIndices.empty()); // never filled by the protocol

	// Keys in query order; within a key, uploaders in RakNetGUID order (the server's sort order).
	const RakNetGUID first = std::min(clients[0]->GetMyGUID(), clients[1]->GetMyGUID());
	EXPECT_EQ(result.rowsReturned[0]->key.secondaryKey, 1u);
	EXPECT_EQ(result.rowsReturned[0]->clientGUID, first);
	EXPECT_EQ(RowData(result.rowsReturned[0]), first == clients[0]->GetMyGUID() ? "alpha" : "bravo");
	EXPECT_EQ(result.rowsReturned[1]->key.secondaryKey, 1u);
	EXPECT_NE(result.rowsReturned[1]->clientGUID, first);
	EXPECT_EQ(result.rowsReturned[2]->key.secondaryKey, 2u);
	EXPECT_EQ(result.rowsReturned[2]->clientGUID, clients[0]->GetMyGUID());
	EXPECT_EQ(RowData(result.rowsReturned[2]), "zulu");

	cloudClients[0].DeallocateWithDefaultAllocator(&result);
	EXPECT_TRUE(result.rowsReturned.empty());
	EXPECT_TRUE(result.cloudQuery.keys.empty());
}

TEST_F(CloudApi, GetForSpecificSystemsTakesAStdVector)
{
	Post(0, Scores(), "alpha");
	Post(1, Scores(), "bravo");
	Settle();

	CloudQuery query;
	query.keys.push_back(Scores());
	std::vector<RakNetGUID> only{clients[1]->GetMyGUID()};
	ASSERT_TRUE(cloudClients[0].Get(&query, only, server->GetMyGUID()));

	CloudQueryResult result;
	ASSERT_TRUE(NextGetResponse(0, result));
	ASSERT_EQ(result.rowsReturned.size(), 1u);
	EXPECT_EQ(RowData(result.rowsReturned[0]), "bravo");

	// The CloudQueryRow overload sends one request with one specific system. (It prefixes each
	// system with a GUID-or-address flag that CloudServer does not read, so the server sees a
	// shifted GUID and matches no uploader; the wire format is frozen, so this is pinned as is.)
	std::vector<CloudQueryRow *> rows{result.rowsReturned[0]};
	ASSERT_TRUE(cloudClients[0].Get(&query, rows, server->GetMyGUID()));
	CloudQueryResult again;
	ASSERT_TRUE(NextGetResponse(0, again));
	EXPECT_TRUE(again.rowsReturned.empty());

	cloudClients[0].DeallocateWithDefaultAllocator(&result);
	cloudClients[0].DeallocateWithDefaultAllocator(&again);
}

TEST_F(CloudApi, ReleaseTakesAStdVectorAndAFilterMayShortenIt)
{
	RecordingFilter filter;
	filter.keepFirstReleasedKeyOnly = true;
	cloudServer.AddQueryFilter(&filter);
	cloudServer.AddQueryFilter(&filter); // already added: ignored

	Post(0, Scores(), "alpha");
	Post(0, Names(), "zulu");
	Settle();

	std::vector<CloudKey> keys{Scores(), Names()};
	cloudClients[0].Release(keys, server->GetMyGUID());
	Settle();
	ASSERT_EQ(filter.releaseKeyCounts, (std::vector<size_t>{2}));

	// The filter kept only Scores(): Names() is still there.
	CloudQuery query;
	query.keys = {Scores(), Names()};
	ASSERT_TRUE(cloudClients[1].Get(&query, server->GetMyGUID()));
	CloudQueryResult result;
	ASSERT_TRUE(NextGetResponse(1, result));
	ASSERT_EQ(result.rowsReturned.size(), 1u);
	EXPECT_EQ(result.rowsReturned[0]->key.secondaryKey, 2u);
	EXPECT_EQ(filter.getKeyCounts, (std::vector<size_t>{2}));
	EXPECT_EQ(filter.getSpecificCounts, (std::vector<size_t>{0}));
	cloudClients[1].DeallocateWithDefaultAllocator(&result);

	cloudServer.RemoveQueryFilter(&filter);
}

TEST_F(CloudApi, RejectedGetGetsNoResponse)
{
	RecordingFilter filter;
	filter.allowGets = false;
	cloudServer.AddQueryFilter(&filter);

	CloudQuery query;
	query.keys.push_back(Scores());
	ASSERT_TRUE(cloudClients[0].Get(&query, server->GetMyGUID()));
	Settle();
	EXPECT_FALSE(net.Received(clients[0], ID_CLOUD_GET_RESPONSE));
	EXPECT_EQ(filter.getKeyCounts.size(), 1u);
	cloudServer.RemoveAllQueryFilters();
}

TEST_F(CloudApi, UnsubscribeTakesStdVectors)
{
	RecordingFilter filter;
	cloudServer.AddQueryFilter(&filter);

	// Client 0 subscribes to Scores() from every uploader.
	CloudQuery query;
	query.keys.push_back(Scores());
	query.subscribeToResults = true;
	ASSERT_TRUE(cloudClients[0].Get(&query, server->GetMyGUID()));
	CloudQueryResult result;
	ASSERT_TRUE(NextGetResponse(0, result));
	EXPECT_TRUE(result.rowsReturned.empty());

	Post(1, Scores(), "bravo");
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_CLOUD_SUBSCRIPTION_NOTIFICATION, 2000));
	EXPECT_TRUE(ReceivedNotification(0));

	// Unsubscribing a specific system from an all-systems subscription is refused...
	std::vector<CloudKey> keys{Scores()};
	std::vector<RakNetGUID> systems{clients[1]->GetMyGUID()};
	cloudClients[0].Unsubscribe(keys, systems, server->GetMyGUID());
	Settle();
	Post(1, Scores(), "bravo2");
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_CLOUD_SUBSCRIPTION_NOTIFICATION, 2000));
	EXPECT_TRUE(ReceivedNotification(0));

	// ...unsubscribing from the key ends it.
	cloudClients[0].Unsubscribe(keys, server->GetMyGUID());
	Settle();
	Post(1, Scores(), "bravo3");
	Settle();
	EXPECT_FALSE(ReceivedNotification(0));
	EXPECT_EQ(filter.unsubscribeCounts, (std::vector<size_t>{101, 100}));

	cloudServer.RemoveAllQueryFilters();
}

TEST_F(CloudApi, SpecificSubscriptionEndsWhenUnsubscribedFromTheSystem)
{
	RecordingFilter filter;
	cloudServer.AddQueryFilter(&filter);

	// Client 0 subscribes to Scores() uploaded by client 1 only.
	CloudQuery query;
	query.keys.push_back(Scores());
	query.subscribeToResults = true;
	std::vector<RakNetGUID> systems{clients[1]->GetMyGUID()};
	ASSERT_TRUE(cloudClients[0].Get(&query, systems, server->GetMyGUID()));
	CloudQueryResult result;
	ASSERT_TRUE(NextGetResponse(0, result));

	Post(1, Scores(), "bravo");
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_CLOUD_SUBSCRIPTION_NOTIFICATION, 2000));
	EXPECT_TRUE(ReceivedNotification(0));

	// The CloudQueryRow overload reaches the server with one key and one system (which, see
	// GetForSpecificSystemsTakesAStdVector, the server reads shifted: the subscription stays).
	CloudQueryRow row;
	row.clientGUID = clients[1]->GetMyGUID();
	std::vector<CloudQueryRow *> rows{&row};
	std::vector<CloudKey> keys{Scores()};
	cloudClients[0].Unsubscribe(keys, rows, server->GetMyGUID());
	Settle();
	EXPECT_EQ(filter.unsubscribeCounts, (std::vector<size_t>{101}));

	// The RakNetGUID overload ends it.
	std::vector<RakNetGUID> specific{clients[1]->GetMyGUID()};
	cloudClients[0].Unsubscribe(keys, specific, server->GetMyGUID());
	Settle();
	Post(1, Scores(), "bravo2");
	Settle();
	EXPECT_FALSE(ReceivedNotification(0));
	EXPECT_EQ(filter.unsubscribeCounts, (std::vector<size_t>{101, 101}));
	cloudServer.RemoveAllQueryFilters();
}

TEST(CloudApiServers, GetIsAnsweredAcrossServers)
{
	CloudServer cloudA, cloudB;
	CloudClient cloudClientA, cloudClientB;
	SimulatedNetwork net; // after the plugins: destroyed (with its peers) first
	RakPeer *serverA = net.AddPeer(8, 8);
	RakPeer *serverB = net.AddPeer(8, 8);
	RakPeer *clientA = net.AddPeer(1);
	RakPeer *clientB = net.AddPeer(1);
	serverA->AttachPlugin(&cloudA);
	serverB->AttachPlugin(&cloudB);
	clientA->AttachPlugin(&cloudClientA);
	clientB->AttachPlugin(&cloudClientB);
	ASSERT_TRUE(net.Connect(serverB, serverA));
	ASSERT_TRUE(net.Connect(clientA, serverA));
	ASSERT_TRUE(net.Connect(clientB, serverB));

	cloudA.AddServer(serverB->GetMyGUID());
	cloudB.AddServer(serverA->GetMyGUID());
	std::vector<RakNetGUID> remote{serverA->GetMyGUID(), serverA->GetMyGUID()}; // replaced, not appended to
	cloudA.GetRemoteServers(remote);
	EXPECT_EQ(remote, (std::vector<RakNetGUID>{serverB->GetMyGUID()}));
	net.RunUntil([]
	             { return false; }, 300);

	CloudKey key = Scores();
	const std::string value = "remote";
	cloudClientB.Post(&key, reinterpret_cast<const unsigned char *>(value.data()), static_cast<uint32_t>(value.size()), serverB->GetMyGUID());
	net.RunUntil([]
	             { return false; }, 300);

	// Two requests in flight at once: each response must find its own request (well before the
	// 3 s timeout that would also answer it, with only the local rows).
	CloudQuery query;
	query.keys.push_back(Scores());
	ASSERT_TRUE(cloudClientA.Get(&query, serverA->GetMyGUID()));
	ASSERT_TRUE(cloudClientA.Get(&query, serverA->GetMyGUID()));
	ASSERT_TRUE(net.RunUntil([&]
	                         { return std::count_if(net.Inbox(clientA).begin(), net.Inbox(clientA).end(), [](const SimulatedNetwork::Message &m)
		                                            { return m.id == ID_CLOUD_GET_RESPONSE; }) == 2; },
	                         2000));
	for (SimulatedNetwork::Message &message : net.Inbox(clientA))
	{
		if (message.id != ID_CLOUD_GET_RESPONSE)
			continue;
		Packet packet = AsPacket(message);
		CloudQueryResult result;
		cloudClientA.OnGetReponse(&result, &packet);
		ASSERT_EQ(result.rowsReturned.size(), 1u);
		EXPECT_EQ(RowData(result.rowsReturned[0]), "remote");
		EXPECT_EQ(result.rowsReturned[0]->clientGUID, clientB->GetMyGUID());
		cloudClientA.DeallocateWithDefaultAllocator(&result);
	}

	clientB->DetachPlugin(&cloudClientB);
	clientA->DetachPlugin(&cloudClientA);
	serverB->DetachPlugin(&cloudB);
	serverA->DetachPlugin(&cloudA);
}

TEST(CloudApiServers, OverLimitPostDoesNotCountAsAnUploader)
{
	CloudServer cloudA, cloudB;
	CloudClient uploader, subscriber, getter;
	SimulatedNetwork net; // after the plugins: destroyed (with its peers) first
	RakPeer *serverA = net.AddPeer(8, 8);
	RakPeer *serverB = net.AddPeer(8, 8);
	RakPeer *uploaderPeer = net.AddPeer(1);
	RakPeer *subscriberPeer = net.AddPeer(1);
	RakPeer *getterPeer = net.AddPeer(1);
	serverA->AttachPlugin(&cloudA);
	serverB->AttachPlugin(&cloudB);
	uploaderPeer->AttachPlugin(&uploader);
	subscriberPeer->AttachPlugin(&subscriber);
	getterPeer->AttachPlugin(&getter);
	ASSERT_TRUE(net.Connect(serverB, serverA));
	ASSERT_TRUE(net.Connect(uploaderPeer, serverA));
	ASSERT_TRUE(net.Connect(subscriberPeer, serverA));
	ASSERT_TRUE(net.Connect(getterPeer, serverB));
	cloudA.SetMaxUploadBytesPerClient(10);
	const auto settle = [&net]
	{ net.RunUntil([]
		           { return false; }, 300); };
	const auto post = [&](const CloudKey &key, const std::string &value)
	{
		CloudKey copy = key;
		uploader.Post(&copy, reinterpret_cast<const unsigned char *>(value.data()), static_cast<uint32_t>(value.size()), serverA->GetMyGUID());
		settle();
	};

	// 8 of the uploader's 10 bytes go to Scores() before the servers meet, so server B learns
	// exactly which keys A holds (and asks A only for those).
	post(Scores(), "12345678");
	cloudA.AddServer(serverB->GetMyGUID());
	cloudB.AddServer(serverA->GetMyGUID());
	settle();

	// Names() exists on A with a subscriber only.
	CloudQuery subscribe;
	subscribe.keys.push_back(Names());
	subscribe.subscribeToResults = true;
	ASSERT_TRUE(subscriber.Get(&subscribe, serverA->GetMyGUID()));
	settle();

	post(Names(), "abcde"); // 8 + 5 > 10: rejected, must leave no uploader behind
	std::vector<CloudKey> release{Scores()};
	uploader.Release(release, serverA->GetMyGUID());
	settle();
	post(Names(), "abcde"); // now within the limit: the first real upload of Names()

	// B forwards a Get for Names() to A only if A announced it as uploaded.
	CloudQuery query;
	query.keys.push_back(Names());
	ASSERT_TRUE(getter.Get(&query, serverB->GetMyGUID()));
	ASSERT_TRUE(net.RunUntilReceived(getterPeer, ID_CLOUD_GET_RESPONSE, 2000));
	SimulatedNetwork::Message message = *net.Find(getterPeer, ID_CLOUD_GET_RESPONSE);
	Packet packet = AsPacket(message);
	CloudQueryResult result;
	getter.OnGetReponse(&result, &packet);
	ASSERT_EQ(result.rowsReturned.size(), 1u);
	EXPECT_EQ(RowData(result.rowsReturned[0]), "abcde");
	getter.DeallocateWithDefaultAllocator(&result);

	getterPeer->DetachPlugin(&getter);
	subscriberPeer->DetachPlugin(&subscriber);
	uploaderPeer->DetachPlugin(&uploader);
	serverB->DetachPlugin(&cloudB);
	serverA->DetachPlugin(&cloudA);
}

namespace
{
// Counts what CloudQueryResult::Serialize allocates while reading peer input. Rows come from
// zero-filled storage, default-initialized in place, so a field a failed read left untouched
// reads as 0 whether or not the row type initializes it. Gives up (returns null) after
// kRowBudget rows or for data larger than 1 MiB, so a runaway reader stops instead of exhausting memory.
struct CountingAllocator : CloudAllocator
{
	static constexpr int kRowBudget = 1000;
	CloudQueryRow *AllocateCloudQueryRow(void) override
	{
		if (rowsAllocated >= kRowBudget)
			return nullptr;
		++rowsAllocated;
		void *storage = ::operator new(sizeof(CloudQueryRow));
		std::memset(storage, 0, sizeof(CloudQueryRow));
		return new (storage) CloudQueryRow;
	}
	void DeallocateCloudQueryRow(CloudQueryRow *row) override
	{
		++rowsFreed;
		row->~CloudQueryRow();
		::operator delete(row);
	}
	unsigned char *AllocateRowData(uint32_t bytes) override
	{
		largestDataRequest = std::max(largestDataRequest, bytes);
		if (bytes > 1024 * 1024)
			return nullptr;
		++dataAllocated;
		return CloudAllocator::AllocateRowData(bytes);
	}
	void DeallocateRowData(void *data) override
	{
		if (data)
			++dataFreed;
		CloudAllocator::DeallocateRowData(data);
	}
	void FreeAll(CloudQueryResult &result)
	{
		for (CloudQueryRow *row : result.rowsReturned)
		{
			DeallocateRowData(row->data);
			DeallocateCloudQueryRow(row);
		}
		result.rowsReturned.clear();
	}
	int rowsAllocated = 0, rowsFreed = 0, dataAllocated = 0, dataFreed = 0;
	uint32_t largestDataRequest = 0;
};

void WriteResponseHeader(BitStream &bs, uint32_t claimedRows)
{
	CloudQueryResult header;
	header.cloudQuery.keys.push_back(Scores());
	header.subscribeToResults = false;
	header.SerializeHeader(true, &bs);
	header.SerializeNumRows(true, claimedRows, &bs);
}

void WriteRow(BitStream &bs, uint32_t claimedLength, const std::string &bytes)
{
	CloudKey key = Scores();
	key.Serialize(true, &bs);
	bs.Write(UNASSIGNED_SYSTEM_ADDRESS);
	bs.Write(UNASSIGNED_SYSTEM_ADDRESS);
	bs.Write(UNASSIGNED_RAKNET_GUID);
	bs.Write(UNASSIGNED_RAKNET_GUID);
	bs.Write(claimedLength);
	bs.WriteAlignedBytes(reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<unsigned int>(bytes.size()));
}
} // namespace

TEST(CloudRows, ClaimedRowCountIsBoundedByTheStream)
{
	// A response that claims 2^32 - 1 rows and holds none: nothing may be allocated for them.
	BitStream bs;
	WriteResponseHeader(bs, 0xFFFFFFFFu);
	CountingAllocator allocator;
	CloudQueryResult result;
	result.Serialize(false, &bs, &allocator);
	EXPECT_TRUE(result.rowsReturned.empty());
	EXPECT_LE(allocator.rowsAllocated, 1);
	EXPECT_EQ(allocator.rowsAllocated, allocator.rowsFreed);
	allocator.FreeAll(result);
}

TEST(CloudRows, TruncatedRowIsDroppedWithoutLeaking)
{
	// Two rows claimed; the second is cut off inside its addresses.
	BitStream whole;
	WriteResponseHeader(whole, 2);
	WriteRow(whole, 2, "hi");
	const BitSize_t secondRowStart = whole.GetNumberOfBitsUsed();
	WriteRow(whole, 3, "abc");
	BitStream bs;
	bs.WriteBits(whole.GetData(), secondRowStart + 40, false);

	CountingAllocator allocator;
	CloudQueryResult result;
	result.Serialize(false, &bs, &allocator);
	ASSERT_EQ(result.rowsReturned.size(), 1u);
	EXPECT_EQ(RowData(result.rowsReturned[0]), "hi");
	EXPECT_EQ(allocator.rowsAllocated - allocator.rowsFreed, 1); // the dropped row was freed
	EXPECT_EQ(allocator.dataAllocated - allocator.dataFreed, 1);
	allocator.FreeAll(result);
}

TEST(CloudRows, RowLengthLargerThanTheStreamIsNotAllocated)
{
	// A row claiming 1 GB of data with 3 bytes behind it: no allocation is attempted for it.
	BitStream bs;
	WriteRow(bs, 1000000000u, "abc");
	CountingAllocator allocator;
	CloudQueryRow row;
	row.Serialize(false, &bs, &allocator);
	EXPECT_EQ(row.data, nullptr);
	EXPECT_LE(allocator.largestDataRequest, 3u);
	allocator.DeallocateRowData(row.data);
}
