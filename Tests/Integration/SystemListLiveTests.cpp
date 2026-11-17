#include <gtest/gtest.h>

#include <thread>

#include "TestHelpers.h"
#include "mafianet/peer_handle.h"

using namespace MafiaNet;

TEST(SystemListLive, AddressGuidPairsStayTogetherDuringSlotReuse)
{
	Peer server;
	const auto serverPort = TestHelpers::StartOnLoopback(server.get(), 2, 2);
	ASSERT_NE(0, serverPort);

	Peer resident;
	const auto residentPort = TestHelpers::StartOnLoopback(resident.get(), 1);
	ASSERT_NE(0, residentPort);
	const SystemAddress residentAddress("127.0.0.1", residentPort);
	const auto residentGuid = resident->GetMyGUID();
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, resident->Connect("127.0.0.1", serverPort, nullptr, 0));

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	auto waitForList = [&](size_t count, Peer *visitor, const SystemAddress &visitorAddress, RakNetGUID visitorGuid)
	{
		const bool closing = visitor && count == 1;
		const auto serverEvent = closing ? ID_DISCONNECTION_NOTIFICATION : ID_NEW_INCOMING_CONNECTION;
		const auto clientEvent = closing ? ID_DISCONNECTION_NOTIFICATION : ID_CONNECTION_REQUEST_ACCEPTED;
		const auto clientGuid = visitor ? visitorGuid : residentGuid;
		bool serverObserved = false;
		bool clientObserved = false;
		const auto start = GetTimeMS();
		do
		{
			for (auto packet : server.incoming())
				if (packet.id() == serverEvent && packet->guid == clientGuid)
					serverObserved = true;
			Peer &client = visitor ? *visitor : resident;
			for (auto packet : client.incoming())
				if (packet.id() == clientEvent && packet->guid == server->GetMyGUID())
					clientObserved = true;
			if (visitor)
				for (auto packet : resident.incoming())
					(void) packet;

			server->GetSystemList(addresses, guids);
			if (addresses.size() != guids.size() || addresses.size() > 2)
				return false;
			bool sawResident = false;
			bool sawVisitor = false;
			for (size_t i = 0; i < addresses.size(); ++i)
			{
				if (!sawResident && addresses[i] == residentAddress && guids[i] == residentGuid)
					sawResident = true;
				else if (!sawVisitor && addresses[i] == visitorAddress && guids[i] == visitorGuid && visitorGuid != UNASSIGNED_RAKNET_GUID)
					sawVisitor = true;
				else
					return false;
			}
			if (!sawResident && visitorGuid != UNASSIGNED_RAKNET_GUID)
				return false;
			if (sawResident && addresses.size() == count && serverObserved && clientObserved)
				return true;
			std::this_thread::yield();
		} while (GetTimeMS() - start < 5000);
		return false;
	};
	ASSERT_TRUE(waitForList(1, nullptr, UNASSIGNED_SYSTEM_ADDRESS, UNASSIGNED_RAKNET_GUID));
	for (int visit = 0; visit < 8; ++visit)
	{
		SCOPED_TRACE(visit);
		Peer visitor;
		const auto port = TestHelpers::StartOnLoopback(visitor.get(), 1);
		ASSERT_NE(0, port);
		const SystemAddress address("127.0.0.1", port);
		const auto guid = visitor->GetMyGUID();
		ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, visitor->Connect("127.0.0.1", serverPort, nullptr, 0));
		ASSERT_TRUE(waitForList(2, &visitor, address, guid));
		server->CloseConnection(address, true);
		ASSERT_TRUE(waitForList(1, &visitor, address, guid));
	}
	server->Shutdown(0);
	server->GetSystemList(addresses, guids);
	EXPECT_TRUE(addresses.empty());
	EXPECT_TRUE(guids.empty());
}
