/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"

using namespace MafiaNet;

/*
Description:
Tests:
virtual unsigned short RakPeerInterface::NumberOfConnections  	(  	void   	  	 )   	 const
virtual void RakPeerInterface::GetSystemList  	(  	DataStructures::List< SystemAddress > &   	 addresses, 		DataStructures::List< RakNetGUID > &  	guids	  	)
virtual bool RakPeerInterface::IsActive  	(  	void   	  	 )   	 const
virtual SystemAddress RakPeerInterface::GetSystemAddressFromIndex  	(  	int   	 index  	 )
virtual SystemAddress RakPeerInterface::GetSystemAddressFromGuid  	(  	const RakNetGUID   	 input  	 )   	 const
virtual const RakNetGUID& RakPeerInterface::GetGuidFromSystemAddress  	(  	const SystemAddress   	 input  	 )   	 const
pure virtual  virtual RakNetGUID RakPeerInterface::GetGUIDFromIndex  	(  	int   	 index  	 )
virtual SystemAddress RakPeerInterface::GetExternalID  	(  	const SystemAddress   	 target  	 )   	 const

Success conditions:
All functions pass test.

Failure conditions:
Any function fails.

RakPeerInterface Functions used, tested indirectly by its use. List may not be complete:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send
IsConnected

RakPeerInterface Functions Explicitly Tested:

NumberOfConnections
GetSystemList
IsActive
GetSystemAddressFromIndex
GetSystemAddressFromGuid
GetGuidFromSystemAddress
GetGUIDFromIndex
GetExternalID

*/

class SystemAddressAndGuid : public ::testing::Test
{
protected:
	void TearDown() override
	{
		int theSize = destroyList.Size();

		// Shutdown all peers before destroying to let threads clean up
		for (int i = 0; i < theSize; i++)
			destroyList[i]->Shutdown(100);

		for (int i = 0; i < theSize; i++)
			RakPeerInterface::DestroyInstance(destroyList[i]);
	}

	DataStructures::List<RakPeerInterface *> destroyList;
};

TEST_F(SystemAddressAndGuid, AddressAndGuidLookups)
{
	RakPeerInterface *server, *client;

	printf("Testing IsActive\n");
	client = RakPeerInterface::GetInstance();
	destroyList.Push(client, _FILE_AND_LINE_);
	ASSERT_FALSE(client->IsActive()) << "Client was active but shouldn't be yet";

	SocketDescriptor clientSd(60001, 0);
	client->Startup(1, &clientSd, 1);

	ASSERT_TRUE(client->IsActive()) << "Client was not active but should be";

	//Passed by reference for initializations
	TestHelpers::StandardServerPrep(server, destroyList);

	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(client, server, 5000)) << "Could not connect the client";

	DataStructures::List<SystemAddress> systemList;
	DataStructures::List<RakNetGUID> guidList;

	printf("Test GetSystemList and NumberOfConnections\n");

	client->GetSystemList(systemList, guidList);//Get connectionlist
	int len = systemList.Size();
	int len2 = guidList.Size();

	int conNum = client->NumberOfConnections();

	printf("Test if systemList size matches guidList size \n");

	ASSERT_EQ(len2, len) << "Mismatch between guidList size and systemList size (system list size is " << len << " and guid size is " << len2 << ")";

	printf("Test returned list size against NumberofConnections return value\n");
	if (conNum != len)
	{
		if (conNum == 1 || len == 1)
		{
			ASSERT_EQ(conNum, 1) << "NumberOfConnections problem (system list size is " << len << " and NumberOfConnections return is " << conNum << ")";

			ASSERT_EQ(len, 1) << "SystemList problem with GetSystemList (system list size is " << len << " and NumberOfConnections return is " << conNum << ")";
		}
		else
		{
			FAIL() << "Both SystemList and Number of connections have problems and report different results (system list size is " << len << " and NumberOfConnections return is " << conNum << ")";
		}
	}
	else
	{
		ASSERT_EQ(conNum, 1) << "Both SystemList and Number of connections have problems and report same results (system list size is " << len << " and NumberOfConnections return is " << conNum << ")";
	}

	printf("Test GetSystemListValues of the system and guid list\n");
	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(60000);

	ASSERT_TRUE(systemList[0] == serverAddress) << "System address from list is wrong.";

	RakNetGUID serverGuid = server->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS);

	ASSERT_FALSE(guidList[0] != serverGuid) << "Guid from list is wrong";

	printf("Test GetSystemAddressFromIndex\n");
	ASSERT_TRUE(client->GetSystemAddressFromIndex(0) == serverAddress) << "GetSystemAddressFromIndex failed to return correct values";

	printf("Test GetSystemAddressFromGuid\n");
	ASSERT_TRUE(client->GetSystemAddressFromGuid(serverGuid) == serverAddress) << "GetSystemAddressFromGuid failed to return correct values";

	printf("Test GetGuidFromSystemAddress\n");
	ASSERT_FALSE(client->GetGuidFromSystemAddress(serverAddress) != serverGuid) << "GetGuidFromSystemAddress failed to return correct values";

	printf("Test GetGUIDFromIndex\n");
	ASSERT_FALSE(client->GetGUIDFromIndex(0) != serverGuid) << "GetGUIDFromIndex failed to return correct values";

	SystemAddress clientAddress;
	clientAddress.SetBinaryAddress("127.0.0.1");
	clientAddress.SetPortHostOrder(60001);

	printf("Test GetExternalID, automatic testing is not only required for this\nbecause of it's nature\nShould be supplemented by internet tests\n");

	ASSERT_TRUE(client->GetExternalID(serverAddress) == clientAddress) << "GetExternalID failed to return correct values";
}
