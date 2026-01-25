/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "Lobby2Message.h"
#include "mafianet/peerinterface.h"

#include "mafianet/MessageIdentifiers.h"
#include "Lobby2Client.h"
#include "mafianet/Kbhit.h"
#include "mafianet/sleep.h"
#include "RoomsErrorCodes.h"
#include "mafianet/DS_Queue.h"
#include <ctype.h>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/LinuxStrings.h"
#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

static const int NUM_CONNECTIONS=2;
MafiaNet::Lobby2Client lobby2Client[NUM_CONNECTIONS];
MafiaNet::Lobby2MessageFactory messageFactory;
MafiaNet::RakString testUserName[NUM_CONNECTIONS];
MafiaNet::RakPeerInterface *rakPeer[NUM_CONNECTIONS];
struct AutoExecutionPlanNode
{
	AutoExecutionPlanNode() {}
	AutoExecutionPlanNode(int i, MafiaNet::Lobby2MessageID o) {instanceNumber=i; operation=o;}
	int instanceNumber;
	MafiaNet::Lobby2MessageID operation;
};
DataStructures::Queue<AutoExecutionPlanNode> executionPlan;

void PrintCommands()
{
	unsigned int i;
	for (i=0; i < MafiaNet::L2MID_COUNT; i++)
	{
		MafiaNet::Lobby2Message *m = messageFactory.Alloc((MafiaNet::Lobby2MessageID)i);
		if (m)
		{
			printf("%i. %s", i+1, m->GetName());
			if (m->RequiresAdmin())
				printf(" (Admin command)");
			if (m->RequiresRankingPermission())
				printf(" (Ranking server command)");
			printf("\n");
			messageFactory.Dealloc(m);
		}		
		
	}
}

void ExecuteCommand(MafiaNet::Lobby2MessageID command, MafiaNet::RakString userName, int instanceNumber);
struct Lobby2ClientSampleCB : public MafiaNet::Lobby2Printf
{
	virtual void ExecuteDefaultResult(MafiaNet::Lobby2Message *message) {
		message->DebugPrintf();
		if (message->resultCode== MafiaNet::REC_SUCCESS && executionPlan.Size())
		{
			AutoExecutionPlanNode aepn = executionPlan.Pop();
			ExecuteCommand(aepn.operation, MafiaNet::RakString("user%i", aepn.instanceNumber), aepn.instanceNumber);
		}
	}
} callback[NUM_CONNECTIONS];

int main()
{
	printf("This sample creates two Lobby2Clients.\n");
	printf("They both connect to the server and performs queued operations on startup.");
	printf("(RANKING AND CLANS NOT YET DONE).\n");
	printf("Difficulty: Advanced\n\n");

	MafiaNet::Lobby2ResultCodeDescription::Validate();

	/// Do all these operations in this order once we are logged in.
	/// This is for easier testing.
	/// This plan will create the database, register two users, and log them both in
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_System_CreateDatabase), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_System_CreateTitle), _FILE_AND_LINE_ );

	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_CDKey_Add), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_System_RegisterProfanity), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Client_RegisterAccount), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Client_RegisterAccount), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_System_SetEmailAddressValidated), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_System_SetEmailAddressValidated), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Client_Login), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Client_Login), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Emails_Send), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Emails_Get), _FILE_AND_LINE_ );
// 	/// Create 2 clans
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_Create), _FILE_AND_LINE_ );
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_Create), _FILE_AND_LINE_ );
// 	// Invite to both
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SendJoinInvitation), _FILE_AND_LINE_ );
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SendJoinInvitation), _FILE_AND_LINE_ );
// 	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_RejectJoinInvitation), _FILE_AND_LINE_ );
// 	// Download invitations this clan has sent
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_DownloadInvitationList), _FILE_AND_LINE_ );

	/*

	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Client_SetPresence), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Client_GetAccountDetails), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Client_PerTitleIntegerStorage), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Client_PerTitleIntegerStorage), _FILE_AND_LINE_ );

	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Client_StartIgnore), _FILE_AND_LINE_ );
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Client_GetIgnoreList), _FILE_AND_LINE_ );

	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Friends_SendInvite), _FILE_AND_LINE_);
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Friends_AcceptInvite), _FILE_AND_LINE_);

	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Ranking_SubmitMatch));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Ranking_SubmitMatch));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Ranking_UpdateRating));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Ranking_GetRating));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Ranking_WipeRatings));
	*/
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_Create), _FILE_AND_LINE_ );
// 	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_Get), _FILE_AND_LINE_ );
	/*
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SetProperties));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SetMyMemberProperties));
	*/
	/*
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SendJoinInvitation));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_WithdrawJoinInvitation));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_DownloadInvitationList));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SendJoinInvitation));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_RejectJoinInvitation));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SendJoinInvitation));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_AcceptJoinInvitation));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SetSubleaderStatus));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_SetMemberRank));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_GrantLeader));
	*/

	/*
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_SendJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_WithdrawJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_AcceptJoinRequest));
	*/

//	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_SendJoinRequest));
//	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_DownloadRequestList));
	// TODO - test from here
	/*
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_RejectJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_AcceptJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_SendJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_AcceptJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_KickAndBlacklistUser));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_SendJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_GetBlacklist));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_UnblacklistUser));
	executionPlan.Push(AutoExecutionPlanNode(1, MafiaNet::L2MID_Clans_SendJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_AcceptJoinRequest));
	executionPlan.Push(AutoExecutionPlanNode(0, MafiaNet::L2MID_Clans_GetMembers));
	*/

	/*
	// TODO
	L2MID_Clans_CreateBoard,
		L2MID_Clans_DestroyBoard,
		L2MID_Clans_CreateNewTopic,
		L2MID_Clans_ReplyToTopic,
		L2MID_Clans_RemovePost,
		L2MID_Clans_GetBoards,
		L2MID_Clans_GetTopics,
		L2MID_Clans_GetPosts,
		*/
	

	char ip[64], serverPort[30], clientPort[30];
	int i;
	for (i=0; i < NUM_CONNECTIONS; i++)
		rakPeer[i]= MafiaNet::RakPeerInterface::GetInstance();
	puts("Enter the rakPeer1 port to listen on");
	clientPort[0]=0;
	const int intClientPort = atoi(clientPort);
	if ((intClientPort < 0) || (intClientPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified client port %d is outside valid bounds [0, %u]", intClientPort, std::numeric_limits<unsigned short>::max());
		return 1;
	}
	MafiaNet::SocketDescriptor socketDescriptor(static_cast<unsigned short>(intClientPort),0);
	Gets(clientPort,sizeof(clientPort));
	if (clientPort[0]==0)
		strcpy_s(clientPort, "0");

	puts("Enter IP to connect to");;
	ip[0]=0;
	Gets(ip,sizeof(ip));
	if (ip[0]==0)
		strcpy_s(ip, "127.0.0.1");

	puts("Enter the port to connect to");
	serverPort[0]=0;
	Gets(serverPort,sizeof(serverPort));
	if (serverPort[0]==0)
		strcpy_s(serverPort, "61111");
	const int intServerPort = atoi(serverPort);
	if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
		return 2;
	}

	for (i=0; i < NUM_CONNECTIONS; i++)
	{
		rakPeer[i]->Startup(1,&socketDescriptor, 1);
		rakPeer[i]->Connect(ip, static_cast<unsigned short>(intServerPort), 0,0);

		rakPeer[i]->AttachPlugin(&lobby2Client[i]);
		lobby2Client[i].SetMessageFactory(&messageFactory);
		lobby2Client[i].SetCallbackInterface(&callback[i]);
		testUserName[i]= MafiaNet::RakString("user%i", i);
	}

	MafiaNet::Packet *packet;
	// Loop for input
	for(;;)
	{
		for (i=0; i < NUM_CONNECTIONS; i++)
		{
			MafiaNet::RakPeerInterface *peer = rakPeer[i];
			for (packet=peer->Receive(); packet; peer->DeallocatePacket(packet), packet=peer->Receive())
			{
				switch (packet->data[0])
				{
				case ID_DISCONNECTION_NOTIFICATION:
					// Connection lost normally
					printf("ID_DISCONNECTION_NOTIFICATION\n");
					break;
				case ID_ALREADY_CONNECTED:
					// Connection lost normally
					printf("ID_ALREADY_CONNECTED\n");
					break;
				case ID_CONNECTION_BANNED: // Banned from this server
					printf("We are banned from this server.\n");
					break;			
				case ID_CONNECTION_ATTEMPT_FAILED:
					printf("Connection attempt failed\n");
					break;
				case ID_NO_FREE_INCOMING_CONNECTIONS:
					// Sorry, the server is full.  I don't do anything here but
					// A real app should tell the user
					printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
					break;
				case ID_INVALID_PASSWORD:
					printf("ID_INVALID_PASSWORD\n");
					break;
				case ID_CONNECTION_LOST:
					// Couldn't deliver a reliable packet - i.e. the other system was abnormally
					// terminated
					printf("ID_CONNECTION_LOST\n");
					break;
				case ID_CONNECTION_REQUEST_ACCEPTED:
					// This tells the rakPeer1 they have connected
					printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
					int j;
					for (j=0; j < NUM_CONNECTIONS; j++)
						lobby2Client[j].SetServerAddress(packet->systemAddress);
					if (i==NUM_CONNECTIONS-1)
					{
						PrintCommands();
						printf("Enter instance number 1 to %i followed by command number.\n", NUM_CONNECTIONS);

						if (executionPlan.Size())
						{
							/// Execute the first command now that both clients have connected.
							AutoExecutionPlanNode aepn = executionPlan.Pop();
							ExecuteCommand(aepn.operation, MafiaNet::RakString("user%i", aepn.instanceNumber), aepn.instanceNumber);
						}
					}
					break;
				case ID_LOBBY2_SERVER_ERROR:
					{
					MafiaNet::BitStream bs(packet->data,packet->length,false);
						bs.IgnoreBytes(2); // ID_LOBBY2_SERVER_ERROR and error code
						printf("ID_LOBBY2_SERVER_ERROR: ");
						if (packet->data[1]== MafiaNet::L2SE_UNKNOWN_MESSAGE_ID)
						{
							unsigned int messageId;
							bs.Read(messageId);
							printf("L2SE_UNKNOWN_MESSAGE_ID %i", messageId);
						}
						else
							printf("Unknown");
						printf("\n");				
					}

					break;
				}
			}
		}
		
		
		// This sleep keeps RakNet responsive
		RakSleep(30);

		if (_kbhit())
		{
			int ch = _getch();
			if (ch <= '0' || ch > '9')
			{
				printf("Bad instance number\n");
				continue;
			}
			int instanceNumber = ch - 1 - '0';
			if (instanceNumber >= NUM_CONNECTIONS)
			{
				printf("Enter between 1 and %i to pick the instance of RakPeer to run\n", 1+NUM_CONNECTIONS);
				continue;
			}

			printf("Enter message number or 'quit' to quit.\n");
			char str[128];
			Gets(str, sizeof(str));
			if (_stricmp(str, "quit")==0)
			{
				printf("Quitting.\n");
				break;
			}
			else
			{
				int command = atoi(str);
				if (command <=0 || command > MafiaNet::L2MID_COUNT)
				{
					printf("Invalid message index %i. Commands:\n", command);
					PrintCommands();
				}
				else
				{
					ExecuteCommand((MafiaNet::Lobby2MessageID)(command-1), MafiaNet::RakString("user%i", instanceNumber), instanceNumber);
				}
			}
		}
	}

	for (i=0; i < NUM_CONNECTIONS; i++)
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer[i]);
	return 0;
}
/// In a real application these parameters would be filled out from application data
/// Here I've just hardcoded everything for fast testing
void ExecuteCommand(MafiaNet::Lobby2MessageID command, MafiaNet::RakString userName, int instanceNumber)
{
	MafiaNet::Lobby2Message *m = messageFactory.Alloc(command);
	RakAssert(m);
	printf("Executing %s (message %i)\n", m->GetName(), command+1);
	// If additional requires are needed to test the command, stick them here
	switch (m->GetID())
	{
	case MafiaNet::L2MID_System_CreateTitle:
		{
			MafiaNet::System_CreateTitle *arg = (MafiaNet::System_CreateTitle *) m;
			arg->requiredAge=22;
			arg->titleName="Test Title Name";
			arg->titleSecretKey="Test secret key";
		}
		break;
	case MafiaNet::L2MID_System_DestroyTitle:
		{
			MafiaNet::System_DestroyTitle *arg = (MafiaNet::System_DestroyTitle *) m;
			arg->titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_System_GetTitleRequiredAge:
		{
			MafiaNet::System_GetTitleRequiredAge *arg = (MafiaNet::System_GetTitleRequiredAge *) m;
			arg->titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_System_GetTitleBinaryData:
		{
			MafiaNet::System_GetTitleBinaryData *arg = (MafiaNet::System_GetTitleBinaryData *) m;
			arg->titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_System_RegisterProfanity:
		{
			MafiaNet::System_RegisterProfanity *arg = (MafiaNet::System_RegisterProfanity *) m;
			arg->profanityWords.Insert("Bodily Functions", _FILE_AND_LINE_ );
			arg->profanityWords.Insert("Racial Epithet", _FILE_AND_LINE_ );
			arg->profanityWords.Insert("Euphemism treadmill", _FILE_AND_LINE_ );
		}
		break;
	case MafiaNet::L2MID_System_BanUser:
		{
			MafiaNet::System_BanUser *arg = (MafiaNet::System_BanUser *) m;
			arg->durationHours=12;
			arg->banReason="Ban Reason";
			arg->userName=userName;
		}
		break;
	case MafiaNet::L2MID_System_UnbanUser:
		{
			MafiaNet::System_UnbanUser *arg = (MafiaNet::System_UnbanUser *) m;
			arg->reason="Unban Reason";
			arg->userName=userName;
		}
		break;
	case MafiaNet::L2MID_CDKey_Add:
		{
			MafiaNet::CDKey_Add *arg = (MafiaNet::CDKey_Add *) m;
			arg->cdKeys.Insert("Test CD Key", _FILE_AND_LINE_ );
			arg->cdKeys.Insert("Test CD Key 2", _FILE_AND_LINE_ );
			arg->titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_CDKey_GetStatus:
		{
			MafiaNet::CDKey_GetStatus *arg = (MafiaNet::CDKey_GetStatus *) m;
			arg->cdKey="Test CD Key";
			arg->titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_CDKey_Use:
		{
			MafiaNet::CDKey_Use *arg = (MafiaNet::CDKey_Use *) m;
			arg->cdKey="Test CD Key";
			arg->titleName="Test Title Name";
			arg->userName=userName;
		}
		break;
	case MafiaNet::L2MID_CDKey_FlagStolen:
		{
			MafiaNet::CDKey_FlagStolen *arg = (MafiaNet::CDKey_FlagStolen *) m;
			arg->cdKey="Test CD Key";
			arg->titleName="Test Title Name";
			arg->wasStolen=true;
		}
		break;
	case MafiaNet::L2MID_Client_Login:
		{
			MafiaNet::Client_Login *arg = (MafiaNet::Client_Login *) m;
			arg->titleName="Test Title Name";
			arg->titleSecretKey="Test secret key";
			arg->userPassword="asdf";
			arg->userName=userName;
		}
		break;
	case MafiaNet::L2MID_Client_SetPresence:
		{
			MafiaNet::Client_SetPresence *arg = (MafiaNet::Client_SetPresence *) m;
			arg->presence.isVisible=true;
			arg->presence.status= MafiaNet::Lobby2Presence::IN_LOBBY;
//			arg->presence.titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_Client_RegisterAccount:
		{
			MafiaNet::Client_RegisterAccount *arg = (MafiaNet::Client_RegisterAccount *) m;
			arg->createAccountParameters.ageInDays=9999;
			arg->createAccountParameters.firstName="Firstname";
			arg->createAccountParameters.lastName="Lastname";
			arg->createAccountParameters.password="asdf";
			arg->createAccountParameters.passwordRecoveryQuestion="1+2=?";
			arg->createAccountParameters.passwordRecoveryAnswer="3";
			arg->createAccountParameters.emailAddress="username@provider.com";
			arg->createAccountParameters.homeCountry="United States";
			arg->createAccountParameters.homeState="california";
			arg->createAccountParameters.sex_male=true;
			arg->userName=userName;
			arg->cdKey="Test CD Key";
			arg->titleName="Test Title Name";
		}
		break;
	case MafiaNet::L2MID_System_SetEmailAddressValidated:
		{
			MafiaNet::System_SetEmailAddressValidated *arg = (MafiaNet::System_SetEmailAddressValidated *) m;
			arg->validated=true;
			arg->userName=userName;
		}
		break;
	case MafiaNet::L2MID_Client_ValidateHandle:
		{
			MafiaNet::Client_ValidateHandle *arg = (MafiaNet::Client_ValidateHandle *) m;
			arg->userName=userName;
		}
		break;

	case MafiaNet::L2MID_System_DeleteAccount:
		{
			MafiaNet::System_DeleteAccount *arg = (MafiaNet::System_DeleteAccount *) m;
			arg->userName=userName;
			arg->password="asdf";
		}
		break;

	case MafiaNet::L2MID_System_PruneAccounts:
		{
			MafiaNet::System_PruneAccounts *arg = (MafiaNet::System_PruneAccounts *) m;
			arg->deleteAccountsNotLoggedInDays=1;
		}
		break;

	case MafiaNet::L2MID_Client_GetEmailAddress:
		{
			MafiaNet::Client_GetEmailAddress *arg = (MafiaNet::Client_GetEmailAddress *) m;
			arg->userName=userName;
		}
		break;

	case MafiaNet::L2MID_Client_GetPasswordRecoveryQuestionByHandle:
		{
			MafiaNet::Client_GetPasswordRecoveryQuestionByHandle *arg = (MafiaNet::Client_GetPasswordRecoveryQuestionByHandle *) m;
			arg->userName=userName;
		}
		break;

	case MafiaNet::L2MID_Client_GetPasswordByPasswordRecoveryAnswer:
		{
			MafiaNet::Client_GetPasswordByPasswordRecoveryAnswer *arg = (MafiaNet::Client_GetPasswordByPasswordRecoveryAnswer *) m;
			arg->userName=userName;
			arg->passwordRecoveryAnswer="3";
		}
		break;

	case MafiaNet::L2MID_Client_ChangeHandle:
		{
			MafiaNet::Client_ChangeHandle *arg = (MafiaNet::Client_ChangeHandle *) m;
			arg->userName=userName;
			arg->newHandle="New user handle";
		}
		break;

	case MafiaNet::L2MID_Client_UpdateAccount:
		{
			// provided for documentation purposes only
			// MafiaNet::Client_UpdateAccount *arg = (MafiaNet::Client_UpdateAccount *) m;
		}
		break;

	case MafiaNet::L2MID_Client_GetAccountDetails:
		{
			// provided for documentation purposes only
			// MafiaNet::Client_GetAccountDetails *arg = (MafiaNet::Client_GetAccountDetails *) m;
		}
		break;

	case MafiaNet::L2MID_Client_StartIgnore:
		{
			MafiaNet::Client_StartIgnore *arg = (MafiaNet::Client_StartIgnore *) m;
			arg->targetHandle=MafiaNet::RakString("user%i", instanceNumber+1);
		}
		break;

	case MafiaNet::L2MID_Client_StopIgnore:
		{
			MafiaNet::Client_StopIgnore *arg = (MafiaNet::Client_StopIgnore *) m;
			arg->targetHandle=MafiaNet::RakString("user%i", instanceNumber+1);
		}
		break;

	case MafiaNet::L2MID_Client_GetIgnoreList:
		{
			// provided for documentation purposes only
			// MafiaNet::Client_GetIgnoreList *arg = (MafiaNet::Client_GetIgnoreList *) m;
		}
		break;

	case MafiaNet::L2MID_Client_PerTitleIntegerStorage:
		{
		MafiaNet::Client_PerTitleIntegerStorage *arg = (MafiaNet::Client_PerTitleIntegerStorage *) m;
			arg->titleName="Test Title Name";
			arg->slotIndex=0;
			arg->conditionValue=1.0;
			arg->addConditionForOperation= MafiaNet::Client_PerTitleIntegerStorage::PTISC_GREATER_THAN;
			arg->inputValue=0.0;
			static int runCount=0;
			if (runCount++%2==0)
				arg->operationToPerform= MafiaNet::Client_PerTitleIntegerStorage::PTISO_WRITE;
			else
				arg->operationToPerform= MafiaNet::Client_PerTitleIntegerStorage::PTISO_READ;
		}
		break;

	case MafiaNet::L2MID_Friends_SendInvite:
		{
		MafiaNet::Friends_SendInvite *arg = (MafiaNet::Friends_SendInvite *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->subject="Friends_SendInvite subject";
			arg->body="Friends_SendInvite body";
		}
		break;

	case MafiaNet::L2MID_Friends_AcceptInvite:
		{
		MafiaNet::Friends_AcceptInvite *arg = (MafiaNet::Friends_AcceptInvite *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", 0);
			arg->subject="Friends_AcceptInvite subject";
			arg->body="Friends_AcceptInvite body";
			arg->emailStatus=0;
		}
		break;

	case MafiaNet::L2MID_Friends_RejectInvite:
		{
		MafiaNet::Friends_RejectInvite *arg = (MafiaNet::Friends_RejectInvite *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", 0);
			arg->subject="L2MID_Friends_RejectInvite subject";
			arg->body="L2MID_Friends_RejectInvite body";
			arg->emailStatus=0;
		}
		break;

	case MafiaNet::L2MID_Friends_GetInvites:
		{
			// provided for documentation purposes only
			// MafiaNet::Friends_GetInvites *arg = (MafiaNet::Friends_GetInvites *) m;
		}
		break;

	case MafiaNet::L2MID_Friends_GetFriends:
		{
			// provided for documentation purposes only
			// MafiaNet::Friends_GetFriends *arg = (MafiaNet::Friends_GetFriends *) m;
		}
		break;

	case MafiaNet::L2MID_Friends_Remove:
		{
		MafiaNet::Friends_Remove *arg = (MafiaNet::Friends_Remove *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", 0);
			arg->subject="L2MID_Friends_Remove subject";
			arg->body="L2MID_Friends_Remove body";
			arg->emailStatus=0;
		}
		break;

	case MafiaNet::L2MID_BookmarkedUsers_Add:
		{
		MafiaNet::BookmarkedUsers_Add *arg = (MafiaNet::BookmarkedUsers_Add *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->type=0;
			arg->description="L2MID_BookmarkedUsers_Add description";
		}
		break;
	case MafiaNet::L2MID_BookmarkedUsers_Remove:
		{
		MafiaNet::BookmarkedUsers_Remove *arg = (MafiaNet::BookmarkedUsers_Remove *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->type=0;
		}
		break;
	case MafiaNet::L2MID_BookmarkedUsers_Get:
		{
			// provided for documentation purposes only
			// MafiaNet::BookmarkedUsers_Get *arg = (MafiaNet::BookmarkedUsers_Get *) m;
		}
		break;

	case MafiaNet::L2MID_Emails_Send:
		{
		MafiaNet::Emails_Send *arg = (MafiaNet::Emails_Send *) m;
			arg->recipients.Insert(MafiaNet::RakString("user%i", instanceNumber+1), _FILE_AND_LINE_ );
			arg->recipients.Insert(MafiaNet::RakString("user%i", instanceNumber+2), _FILE_AND_LINE_ );
			arg->subject="L2MID_Emails_Send subject";
			arg->body="L2MID_Emails_Send body";
			arg->status=0;
		}
		break;

	case MafiaNet::L2MID_Emails_Get:
		{
		MafiaNet::Emails_Get *arg = (MafiaNet::Emails_Get *) m;
			arg->unreadEmailsOnly=true;
			arg->emailIdsOnly=true;
		}
		break;

	case MafiaNet::L2MID_Emails_Delete:
		{
		MafiaNet::Emails_Delete *arg = (MafiaNet::Emails_Delete *) m;
			arg->emailId=1;
		}
		break;

	case MafiaNet::L2MID_Emails_SetStatus:
		{
		MafiaNet::Emails_SetStatus *arg = (MafiaNet::Emails_SetStatus *) m;
			arg->emailId=2;
			arg->updateStatusFlag=true;
			arg->updateMarkedRead=true;
			arg->newStatusFlag=1234;
			arg->isNowMarkedRead=true;
		}
		break;

	case MafiaNet::L2MID_Ranking_SubmitMatch:
		{
		MafiaNet::Ranking_SubmitMatch *arg = (MafiaNet::Ranking_SubmitMatch *) m;
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
			arg->submittedMatch.matchNote="Ranking match note";
			arg->submittedMatch.matchParticipants.Insert(MafiaNet::MatchParticipant("user0", 5.0f), _FILE_AND_LINE_ );
			arg->submittedMatch.matchParticipants.Insert(MafiaNet::MatchParticipant("user1", 10.0f), _FILE_AND_LINE_ );
		}
		break;

	case MafiaNet::L2MID_Ranking_GetMatches:
		{
		MafiaNet::Ranking_GetMatches *arg = (MafiaNet::Ranking_GetMatches *) m;
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
		}
		break;

	case MafiaNet::L2MID_Ranking_GetMatchBinaryData:
		{
		MafiaNet::Ranking_GetMatchBinaryData *arg = (MafiaNet::Ranking_GetMatchBinaryData *) m;
			arg->matchID=1;
		}
		break;

	case MafiaNet::L2MID_Ranking_GetTotalScore:
		{
		MafiaNet::Ranking_GetTotalScore *arg = (MafiaNet::Ranking_GetTotalScore *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber);
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
		}
		break;

	case MafiaNet::L2MID_Ranking_WipeScoresForPlayer:
		{
		MafiaNet::Ranking_WipeScoresForPlayer *arg = (MafiaNet::Ranking_WipeScoresForPlayer *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber);
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
		}
		break;

	case MafiaNet::L2MID_Ranking_WipeMatches:
		{
		MafiaNet::Ranking_WipeMatches *arg = (MafiaNet::Ranking_WipeMatches *) m;
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
		}
		break;

	case MafiaNet::L2MID_Ranking_PruneMatches:
		{
		MafiaNet::Ranking_PruneMatches *arg = (MafiaNet::Ranking_PruneMatches *) m;
			arg->pruneTimeDays=1;
		}
		break;

	case MafiaNet::L2MID_Ranking_UpdateRating:
		{
		MafiaNet::Ranking_UpdateRating *arg = (MafiaNet::Ranking_UpdateRating *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber);
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
			arg->targetRating=1234.0f;
		}
		break;

	case MafiaNet::L2MID_Ranking_WipeRatings:
		{
		MafiaNet::Ranking_WipeRatings *arg = (MafiaNet::Ranking_WipeRatings *) m;
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
		}
		break;

	case MafiaNet::L2MID_Ranking_GetRating:
		{
		MafiaNet::Ranking_GetRating *arg = (MafiaNet::Ranking_GetRating *) m;
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber);
			arg->gameType="Match game type";
			arg->titleName="Test Title Name";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber);
		}
		break;

	case MafiaNet::L2MID_Clans_Create:
		{
		MafiaNet::Clans_Create *arg = (MafiaNet::Clans_Create *) m;
			static int idx=0;
			arg->clanHandle= MafiaNet::RakString("Clan handle %i", idx++);
			arg->failIfAlreadyInClan=false;
			arg->requiresInvitationsToJoin=true;
			arg->description="Clan Description";
			arg->binaryData->binaryData=new char[10];
			strcpy_s(arg->binaryData->binaryData,arg->binaryData->binaryDataLength,"Hello");
			arg->binaryData->binaryDataLength=10;
		}
		break;

	case MafiaNet::L2MID_Clans_SetProperties:
		{
		MafiaNet::Clans_SetProperties *arg = (MafiaNet::Clans_SetProperties *) m;
			arg->clanHandle="Clan handle";
			arg->description="Updated description";
		}
		break;

	case MafiaNet::L2MID_Clans_GetProperties:
		{
		MafiaNet::Clans_GetProperties *arg = (MafiaNet::Clans_GetProperties *) m;
			arg->clanHandle="Clan handle";
		}
		break;

	case MafiaNet::L2MID_Clans_SetMyMemberProperties:
		{
		MafiaNet::Clans_SetMyMemberProperties *arg = (MafiaNet::Clans_SetMyMemberProperties *) m;
			arg->clanHandle="Clan handle";
			arg->description="Updated description";
		}
		break;

	case MafiaNet::L2MID_Clans_GrantLeader:
		{
		MafiaNet::Clans_GrantLeader *arg = (MafiaNet::Clans_GrantLeader *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
		}
		break;

	case MafiaNet::L2MID_Clans_SetSubleaderStatus:
		{
		MafiaNet::Clans_SetSubleaderStatus *arg = (MafiaNet::Clans_SetSubleaderStatus *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->setToSubleader=true;
		}
		break;

	case MafiaNet::L2MID_Clans_SetMemberRank:
		{
		MafiaNet::Clans_SetMemberRank *arg = (MafiaNet::Clans_SetMemberRank *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->newRank=666;
		}
		break;

	case MafiaNet::L2MID_Clans_GetMemberProperties:
		{
		MafiaNet::Clans_GetMemberProperties *arg = (MafiaNet::Clans_GetMemberProperties *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber);
		}
		break;

	case MafiaNet::L2MID_Clans_ChangeHandle:
		{
		MafiaNet::Clans_ChangeHandle *arg = (MafiaNet::Clans_ChangeHandle *) m;
			arg->oldClanHandle="Clan handle";
			arg->newClanHandle="New Clan handle";
		}
		break;

	case MafiaNet::L2MID_Clans_Leave:
		{
		MafiaNet::Clans_Leave *arg = (MafiaNet::Clans_Leave *) m;
			arg->clanHandle="Clan handle";
			arg->dissolveIfClanLeader=false;
			arg->subject="L2MID_Clans_Leave";
			arg->emailStatus=0;
		}
		break;

	case MafiaNet::L2MID_Clans_Get:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_Get *arg = (MafiaNet::Clans_Get *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_SendJoinInvitation:
		{
		MafiaNet::Clans_SendJoinInvitation *arg = (MafiaNet::Clans_SendJoinInvitation *) m;
			static int idx=0;
			arg->clanHandle= MafiaNet::RakString("Clan handle %i", idx++);
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->subject="L2MID_Clans_SendJoinInvitation";
		}
		break;

	case MafiaNet::L2MID_Clans_WithdrawJoinInvitation:
		{
		MafiaNet::Clans_WithdrawJoinInvitation *arg = (MafiaNet::Clans_WithdrawJoinInvitation *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->subject="L2MID_Clans_WithdrawJoinInvitation";
		}
		break;

	case MafiaNet::L2MID_Clans_AcceptJoinInvitation:
		{
		MafiaNet::Clans_AcceptJoinInvitation *arg = (MafiaNet::Clans_AcceptJoinInvitation *) m;
			static int idx=0;
			arg->clanHandle= MafiaNet::RakString("Clan handle %i", idx++);
			arg->subject="L2MID_Clans_AcceptJoinInvitation";
			arg->failIfAlreadyInClan=false;
		}
		break;

	case MafiaNet::L2MID_Clans_RejectJoinInvitation:
		{
		MafiaNet::Clans_RejectJoinInvitation *arg = (MafiaNet::Clans_RejectJoinInvitation *) m;
			static int idx=0;
			arg->clanHandle= MafiaNet::RakString("Clan handle %i", idx++);
			arg->subject="L2MID_Clans_WithdrawJoinInvitation";
		}
		break;

	case MafiaNet::L2MID_Clans_DownloadInvitationList:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_DownloadInvitationList *arg = (MafiaNet::Clans_DownloadInvitationList *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_SendJoinRequest:
		{
		MafiaNet::Clans_SendJoinRequest *arg = (MafiaNet::Clans_SendJoinRequest *) m;
			arg->clanHandle="Clan handle";
			arg->subject="L2MID_Clans_SendJoinRequest";
		}
		break;

	case MafiaNet::L2MID_Clans_WithdrawJoinRequest:
		{
		MafiaNet::Clans_WithdrawJoinRequest *arg = (MafiaNet::Clans_WithdrawJoinRequest *) m;
			arg->clanHandle="Clan handle";
			arg->subject="L2MID_Clans_WithdrawJoinRequest";
		}
		break;

	case MafiaNet::L2MID_Clans_AcceptJoinRequest:
		{
		MafiaNet::Clans_AcceptJoinRequest *arg = (MafiaNet::Clans_AcceptJoinRequest *) m;
			arg->clanHandle="Clan handle";
			arg->requestingUserHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->subject="L2MID_Clans_AcceptJoinRequest";
		}
		break;

	case MafiaNet::L2MID_Clans_RejectJoinRequest:
		{
		MafiaNet::Clans_RejectJoinRequest *arg = (MafiaNet::Clans_RejectJoinRequest *) m;
			arg->clanHandle="Clan handle";
			arg->requestingUserHandle= MafiaNet::RakString("user%i", instanceNumber+1);
		}
		break;

	case MafiaNet::L2MID_Clans_DownloadRequestList:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_DownloadRequestList *arg = (MafiaNet::Clans_DownloadRequestList *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_KickAndBlacklistUser:
		{
		MafiaNet::Clans_KickAndBlacklistUser *arg = (MafiaNet::Clans_KickAndBlacklistUser *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
			arg->kick=true;
			arg->blacklist=true;
		}
		break;

	case MafiaNet::L2MID_Clans_UnblacklistUser:
		{
		MafiaNet::Clans_UnblacklistUser *arg = (MafiaNet::Clans_UnblacklistUser *) m;
			arg->clanHandle="Clan handle";
			arg->targetHandle= MafiaNet::RakString("user%i", instanceNumber+1);
		}
		break;

	case MafiaNet::L2MID_Clans_GetBlacklist:
		{
		MafiaNet::Clans_GetBlacklist *arg = (MafiaNet::Clans_GetBlacklist *) m;
			arg->clanHandle="Clan handle";
		}
		break;

	case MafiaNet::L2MID_Clans_GetMembers:
		{
		MafiaNet::Clans_GetMembers *arg = (MafiaNet::Clans_GetMembers *) m;
			arg->clanHandle="Clan handle";
		}
		break;

	case MafiaNet::L2MID_Clans_CreateBoard:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_CreateBoard *arg = (MafiaNet::Clans_CreateBoard *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_DestroyBoard:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_DestroyBoard *arg = (MafiaNet::Clans_DestroyBoard *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_CreateNewTopic:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_CreateNewTopic *arg = (MafiaNet::Clans_CreateNewTopic *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_ReplyToTopic:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_ReplyToTopic *arg = (MafiaNet::Clans_ReplyToTopic *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_RemovePost:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_RemovePost *arg = (MafiaNet::Clans_RemovePost *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_GetBoards:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_GetBoards *arg = (MafiaNet::Clans_GetBoards *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_GetTopics:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_GetTopics *arg = (MafiaNet::Clans_GetTopics *) m;
		}
		break;

	case MafiaNet::L2MID_Clans_GetPosts:
		{
			// provided for documentation purposes only
			// MafiaNet::Clans_GetPosts *arg = (MafiaNet::Clans_GetPosts *) m;
		}
		break;
	}
	lobby2Client[instanceNumber].SendMsg(m);
	messageFactory.Dealloc(m);
}
