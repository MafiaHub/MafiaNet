/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"
#include "CommonFunctions.h"

#include <string.h>

using namespace MafiaNet;

/*
Description:

Tests:
virtual void RakPeerInterface::AddToSecurityExceptionList  	(  	const char *   	 ip  	 )
virtual void RakPeerInterface::AddToBanList  	(  	const char *   	 IP, 		TimeMS  	milliseconds = 0	  	)
virtual void RakPeerInterface::GetIncomingPassword  	(  	char *   	 passwordData, 		int *  	passwordDataLength	  	)
virtual void RakPeerInterface::InitializeSecurity  	(  	const char *   	 pubKeyE, 		const char *  	pubKeyN, 		const char *  	privKeyP, 		const char *  	privKeyQ	  	)
virtual bool RakPeerInterface::IsBanned  	(  	const char *   	 IP  	 )
virtual bool RakPeerInterface::IsInSecurityExceptionList  	(  	const char *   	 ip  	 )
virtual void RakPeerInterface::RemoveFromSecurityExceptionList  	(  	const char *   	 ip  	 )
virtual void RakPeerInterface::RemoveFromBanList  	(  	const char *   	 IP  	 )
virtual void RakPeerInterface::SetIncomingPassword  	(  	const char *   	 passwordData, 		int  	passwordDataLength	  	)
virtual void 	ClearBanList (void)=0

Success conditions:
All functions pass tests.

Failure conditions:
Any function fails test.

RakPeerInterface Functions used, tested indirectly by its use:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send
IsConnected
GetStatistics

RakPeerInterface Functions Explicitly Tested:
SetIncomingPassword
GetIncomingPassword
AddToBanList
IsBanned
RemoveFromBanList
ClearBanList
InitializeSecurity  //Disabled because of RakNetStatistics changes
AddToSecurityExceptionList  //Disabled because of RakNetStatistics changes
IsInSecurityExceptionList //Disabled because of RakNetStatistics changes
RemoveFromSecurityExceptionList //Disabled because of RakNetStatistics changes

*/
class SecurityFunctions : public ::testing::Test
{
protected:
	void TearDown() override
	{
		// Shutdown all peers before destroying to let threads clean up
		if (client)
			client->Shutdown(100);
		if (server)
			server->Shutdown(100);

		if (client)
			RakPeerInterface::DestroyInstance(client);
		if (server)
			RakPeerInterface::DestroyInstance(server);
	}

	RakPeerInterface *server = nullptr;
	RakPeerInterface *client = nullptr;
};

TEST_F(SecurityFunctions, PasswordAndBanList)
{
	char thePassword[]="password";
	server=RakPeerInterface::GetInstance();

	client=RakPeerInterface::GetInstance();

	SocketDescriptor clientSd;
	client->Startup(1, &clientSd, 1);
	SocketDescriptor serverSd(60000, 0);
	server->Startup(1, &serverSd, 1);
	server->SetMaximumIncomingConnections(1);
	server->SetIncomingPassword(thePassword,(int)strlen(thePassword));

	char returnedPass[22];
	int returnedLen=22;
	server->GetIncomingPassword(returnedPass,&returnedLen);
	returnedPass[returnedLen]=0;//Password is a data block convert to null terminated string to make the test easier

	ASSERT_STREQ(returnedPass,thePassword) << "GetIncomingPassword returned wrong password";

	SystemAddress serverAddress;

	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(60000);
	TimeMS entryTime=GetTimeMS();

	// Testing if no password is rejected
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),0,0);
		}

		RakSleep(100);

	}

	ASSERT_FALSE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client connected with no password";

	// Testing if incorrect password is rejected
	char badPass[]="badpass";
	entryTime=GetTimeMS();
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),badPass,(int)strlen(badPass));
		}

		RakSleep(100);

	}

	ASSERT_FALSE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client connected with wrong password";

	// Testing if correct password is accepted
	entryTime=GetTimeMS();
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),thePassword,(int)strlen(thePassword));
		}

		RakSleep(100);

	}

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client failed to connect with correct password";

	while(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))//disconnect client
	{

		client->CloseConnection (serverAddress,true,0,MafiaNet::Priority::Low);
	}

	// Testing if connection is rejected after adding to ban list
	server->AddToBanList("127.0.0.1",0);

	entryTime=GetTimeMS();
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),thePassword,(int)strlen(thePassword));
		}

		RakSleep(100);

	}

	ASSERT_TRUE(server->IsBanned("127.0.0.1")) << "IsBanned does not show localhost as banned";

	ASSERT_FALSE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client was banned but connected anyways";

	// Testing if connection is accepted after ban removal by RemoveFromBanList
	server->RemoveFromBanList("127.0.0.1");
	ASSERT_FALSE(server->IsBanned("127.0.0.1")) << "Localhost was not unbanned";

	entryTime=GetTimeMS();
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),thePassword,(int)strlen(thePassword));
		}

		RakSleep(100);

	}

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client failed to connect after banlist removal";

	while(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))//disconnect client
	{

		client->CloseConnection (serverAddress,true,0,MafiaNet::Priority::Low);
	}

	// Testing if connection is rejected after adding to ban list
	server->AddToBanList("127.0.0.1",0);

	entryTime=GetTimeMS();
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),thePassword,(int)strlen(thePassword));
		}

		RakSleep(100);

	}

	ASSERT_TRUE(server->IsBanned("127.0.0.1")) << "IsBanned does not show localhost as banned";

	ASSERT_FALSE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client was banned but connected anyways";

	// Testing if connection is accepted after ban removal by ClearBanList
	server->ClearBanList();
	ASSERT_FALSE(server->IsBanned("127.0.0.1")) << "Localhost was not unbanned";

	entryTime=GetTimeMS();
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),thePassword,(int)strlen(thePassword));
		}

		RakSleep(100);

	}

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client failed to connect after banlist removal with clear function";

	while(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))//disconnect client
	{

		client->CloseConnection (serverAddress,true,0,MafiaNet::Priority::Low);
	}
}
