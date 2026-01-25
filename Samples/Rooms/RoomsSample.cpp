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

#include "RoomsPlugin.h"
#include "mafianet/peerinterface.h"

#include "ProfanityFilter.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/Kbhit.h"
#include <ctype.h>
#include "mafianet/Gets.h"

struct SampleCallbacks : public MafiaNet::RoomsCallback
{
	// Results of calls
	virtual void CreateRoom_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::CreateRoom_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void EnterRoom_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::EnterRoom_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void JoinByFilter_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::JoinByFilter_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void LeaveRoom_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::LeaveRoom_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void GetInvitesToParticipant_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::GetInvitesToParticipant_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SendInvite_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SendInvite_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void AcceptInvite_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::AcceptInvite_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void StartSpectating_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::StartSpectating_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void StopSpectating_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::StopSpectating_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void GrantModerator_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::GrantModerator_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void ChangeSlotCounts_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::ChangeSlotCounts_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SetCustomRoomProperties_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SetCustomRoomProperties_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void GetRoomProperties_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::GetRoomProperties_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void ChangeRoomName_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::ChangeRoomName_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SetHiddenFromSearches_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SetHiddenFromSearches_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SetDestroyOnModeratorLeave_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SetDestroyOnModeratorLeave_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SetReadyStatus_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SetReadyStatus_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void GetReadyStatus_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::GetReadyStatus_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SetRoomLockState_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SetRoomLockState_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void GetRoomLockState_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::GetRoomLockState_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void AreAllMembersReady_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::AreAllMembersReady_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void KickMember_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::KickMember_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void UnbanMember_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::UnbanMember_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void GetBanReason_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::GetBanReason_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void AddUserToQuickJoin_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::AddUserToQuickJoin_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void RemoveUserFromQuickJoin_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RemoveUserFromQuickJoin_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void IsInQuickJoin_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::IsInQuickJoin_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void SearchByFilter_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SearchByFilter_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void ChangeHandle_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::ChangeHandle_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	virtual void Chat_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::Chat_Func *callResult) {(void) senderAddress; callResult->PrintResult();}
	// Notifications due to other room members
	virtual void QuickJoinExpired_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::QuickJoinExpired_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void QuickJoinEnteredRoom_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::QuickJoinEnteredRoom_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberStartedSpectating_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberStartedSpectating_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberStoppedSpectating_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberStoppedSpectating_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void ModeratorChanged_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::ModeratorChanged_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void SlotCountsSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::SlotCountsSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void CustomRoomPropertiesSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::CustomRoomPropertiesSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomNameSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomNameSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void HiddenFromSearchesSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::HiddenFromSearchesSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberReadyStatusSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberReadyStatusSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomLockStateSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomLockStateSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberKicked_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberKicked_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberHandleSet_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberHandleSet_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberLeftRoom_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberLeftRoom_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomMemberJoinedRoom_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomMemberJoinedRoom_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomInvitationSent_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomInvitationSent_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomInvitationWithdrawn_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomInvitationWithdrawn_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void RoomDestroyedOnModeratorLeft_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::RoomDestroyedOnModeratorLeft_Notification *notification) {(void) senderAddress; notification->PrintResult();}
	virtual void Chat_Callback( const MafiaNet::SystemAddress &senderAddress, MafiaNet::Chat_Notification *notification) {(void) senderAddress; notification->PrintResult(); printf("Chat=%s\nFiltered=%s\n", notification->chatMessage.C_String(), notification->filteredChatMessage.C_String());}
};

static const char *GAME_IDENTIFIER="My Game";
static const char *ROOM_NAME="My Room";

void GetUserName(MafiaNet::RoomsPluginFunc *func)
{
	printf("Which user? 1=User1, 2=User2, 3=User3, 4=User4: ");
	int ch = _getche();
	if (ch=='1')
		func->userName="User1";
	else if (ch=='2')
		func->userName="User2";
	else if (ch=='3')
		func->userName="User3";
	else
		func->userName="User4";
	printf("\n");
}
void GetRoomName(MafiaNet::RakString &dest)
{
	char buff[256];
	printf("Enter room name, or enter for default: ");
	Gets(buff,sizeof(buff));
	if (buff[0]==0)
		dest=ROOM_NAME;
	else
		dest=buff;
}
void GetTargetName(MafiaNet::RakString *target)
{
	printf("Which target user? 1=User1, 2=User2, 3=User3, 4=User4: ");
	int ch = _getche();
	if (ch=='1')
		*target="User1";
	else if (ch=='2')
		*target="User2";
	else if (ch=='3')
		*target="User3";
	else
		*target="User4";
	printf("\n");
}
void main(void)
{
	printf("A system for creating rooms for players to meet in before starting games.\n");
	printf("Difficulty: Intermediate\n\n");

	// Do the unit test to make sure the core functionality is correct. The plugin just does networking
	//MafiaNet::AllGamesRoomsContainer::UnitTest();

	MafiaNet::RakPeerInterface *client, *server;
	MafiaNet::RoomsPlugin roomsPluginClient, roomsPluginServer;
	client = MafiaNet::RakPeerInterface::GetInstance();
	server = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::SocketDescriptor sd1(0,0),sd2(1234,0);
	MafiaNet::StartupResult sr;
	sr = client->Startup(1,&sd1, 1);
	RakAssert(sr== MafiaNet::RAKNET_STARTED);
	sr=server->Startup(1,&sd2, 1);
	RakAssert(sr== MafiaNet::RAKNET_STARTED);
	server->SetMaximumIncomingConnections(1);
	client->AttachPlugin(&roomsPluginClient);
	server->AttachPlugin(&roomsPluginServer);
	MafiaNet::ProfanityFilter profanityFilter;
	profanityFilter.AddWord("Crapola");
	roomsPluginServer.SetProfanityFilter(&profanityFilter);
	roomsPluginServer.roomsContainer.AddTitle(GAME_IDENTIFIER);
	SampleCallbacks sampleCallbacks;
	MafiaNet::SystemAddress localAddress=server->GetMyBoundAddress();
	roomsPluginClient.SetServerAddress(localAddress);
	roomsPluginClient.SetRoomsCallback(&sampleCallbacks);
	SLNET_VERIFY(client->Connect("127.0.0.1", 1234, 0, 0, 0) == MafiaNet::CONNECTION_ATTEMPT_STARTED);

	printf("A. CreateRoom\n");
	printf("B. EnterRoom\n");
	printf("C. JoinByFilter\n");
	printf("D. LeaveRoom\n");
	printf("E. GetInvitesToParticipant\n");
	printf("F. SendInvite\n");
	printf("G. AcceptInvite\n");
	printf("H. StartSpectating\n");
	printf("I. StopSpectating\n");
	printf("J. GrantModerator\n");
	printf("K. ChangeSlotCounts\n");
	printf("L. SetCustomRoomProperties\n");
	printf("M. ChangeRoomName\n");
	printf("N. SetHiddenFromSearches\n");
	printf("O. SetDestroyOnModeratorLeave\n");
	printf("P. SetReadyStatus\n");
	printf("Q. GetReadyStatus\n");
	printf("R. SetRoomLockState\n");
	printf("S. GetRoomLockState\n");
	printf("T. AreAllMembersReady\n");
	printf("U. KickMember\n");
	printf("V. UnbanMember\n");
	printf("W. GetBanReason\n");
	printf("X. AddUserToQuickJoin\n");
	printf("Y. RemoveUserFromQuickJoin\n");
	printf("Z. IsInQuickJoin\n");
	printf("1. SearchByFilter\n");
	printf("2. ChangeHandle\n");
	printf("3. RoomChat\n");
	printf("4. GetRoomProperties\n");

	MafiaNet::Packet *p;
	int ch;
	for(;;)
	{
		p=client->Receive();
		if (p)
		{
			if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
			else if (p->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
				printf("ID_CONNECTION_ATTEMPT_FAILED\n");
			else if (p->data[0]==ID_NO_FREE_INCOMING_CONNECTIONS)
				printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
			client->DeallocatePacket(p);
		}
		p=server->Receive();
		if (p)
		{
			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
			{
				roomsPluginServer.LoginRoomsParticipant("User1", p->systemAddress, p->guid, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
				roomsPluginServer.LoginRoomsParticipant("User2", p->systemAddress, p->guid, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
				roomsPluginServer.LoginRoomsParticipant("User3", p->systemAddress, p->guid, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
				roomsPluginServer.LoginRoomsParticipant("User4", p->systemAddress, p->guid, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
			}
			server->DeallocatePacket(p);
		}
		
		if (_kbhit())
		{
			ch = _getch();
			switch (toupper(ch))
			{
			case 'A':
				{
					printf("CreateRoom\n");
					MafiaNet::CreateRoom_Func func;
					GetUserName(&func);
					GetRoomName(func.networkedRoomCreationParameters.roomName);
					func.networkedRoomCreationParameters.slots.publicSlots=1;
					func.gameIdentifier=GAME_IDENTIFIER;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'B':
				{
					printf("EnterRoom\n");
					MafiaNet::EnterRoom_Func func;
					GetUserName(&func);
					// Create or join the specified room name
					GetRoomName(func.networkedRoomCreationParameters.roomName);
					func.query.AddQuery_STRING(
						DefaultRoomColumns::GetColumnName(DefaultRoomColumns::TC_ROOM_NAME),
						func.networkedRoomCreationParameters.roomName.C_String());
					func.networkedRoomCreationParameters.slots.publicSlots=2;
					func.roomMemberMode=RMM_PUBLIC;
					func.gameIdentifier=GAME_IDENTIFIER;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'C':
				{
					printf("JoinByFilter\n");
					MafiaNet::JoinByFilter_Func func;
					GetUserName(&func);
					MafiaNet::RakString roomName;
					GetRoomName(roomName);
					func.query.AddQuery_STRING(
						DefaultRoomColumns::GetColumnName(DefaultRoomColumns::TC_ROOM_NAME),
						roomName.C_String());
					func.gameIdentifier=GAME_IDENTIFIER;
					func.roomMemberMode=RMM_PUBLIC;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'D':
				{
					printf("LeaveRoom\n");
					MafiaNet::LeaveRoom_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'E':
				{
					printf("GetInvitesToParticipant\n");
					MafiaNet::GetInvitesToParticipant_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'F':
				{
					printf("SendInvite\n");
					MafiaNet::SendInvite_Func func;
					GetUserName(&func);
					GetTargetName(&func.inviteeName);
					func.inviteToSpectatorSlot=false;
					func.subject="SendInviteSubject";
					func.body="SendInviteSubject";
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'G':
				{
					printf("AcceptInvite\n");
					MafiaNet::AcceptInvite_Func func;
					GetUserName(&func);
					GetTargetName(&func.inviteSender);
					func.roomId=1;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'H':
				{
					printf("StartSpectating\n");
					MafiaNet::StartSpectating_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'I':
				{
					printf("StopSpectating\n");
					MafiaNet::StopSpectating_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'J':
				{
					printf("GrantModerator\n");
					MafiaNet::GrantModerator_Func func;
					GetUserName(&func);
					GetTargetName(&func.newModerator);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'K':
				{
					printf("ChangeSlotCounts\n");
					MafiaNet::ChangeSlotCounts_Func func;
					GetUserName(&func);
					func.slots.publicSlots=1;
					func.slots.reservedSlots=0;
					func.slots.spectatorSlots=1;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'L':
				{
					printf("SetCustomRoomProperties\n");
					MafiaNet::SetCustomRoomProperties_Func func;
					GetUserName(&func);
					DataStructures::Table customRoomProperties;
					func.table.AddColumn("Custom Column 1", DataStructures::Table::STRING);
					func.table.AddColumn("Custom Column 2", DataStructures::Table::NUMERIC);
					DataStructures::Table::Row* row = func.table.AddRow(0);
					row->cells[0]->Set("Custom Column 1 value");
					row->cells[1]->Set(12345);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'M':
				{
					printf("ChangeRoomName\n");
					MafiaNet::ChangeRoomName_Func func;
					GetUserName(&func);
					func.newRoomName="New room name";
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'N':
				{
					printf("SetHiddenFromSearches\n");
					MafiaNet::SetHiddenFromSearches_Func func;
					GetUserName(&func);
					func.hiddenFromSearches=true;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'O':
				{
					printf("SetDestroyOnModeratorLeave\n");
					MafiaNet::SetDestroyOnModeratorLeave_Func func;
					GetUserName(&func);
					func.destroyOnModeratorLeave=true;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'P':
				{
					printf("SetReadyStatus\n");
					MafiaNet::SetReadyStatus_Func func;
					GetUserName(&func);
					func.isReady=true;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'Q':
				{
					printf("GetReadyStatus\n");
					MafiaNet::GetReadyStatus_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'R':
				{
					printf("SetRoomLockState\n");
					MafiaNet::SetRoomLockState_Func func;
					GetUserName(&func);
					func.roomLockState= MafiaNet::RLS_ALL_LOCKED;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'S':
				{
					printf("GetRoomLockState\n");
					MafiaNet::GetRoomLockState_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'T':
				{
					printf("AreAllMembersReady\n");
					MafiaNet::AreAllMembersReady_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'U':
				{
					printf("KickMember\n");
					MafiaNet::KickMember_Func func;
					GetUserName(&func);
					GetTargetName(&func.kickedMember);
					func.reason="KickMemberReason";
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'V':
				{
					printf("UnbanMember\n");
					MafiaNet::UnbanMember_Func func;
					GetUserName(&func);
					GetTargetName(&func.bannedMemberName);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'W':
				{
					printf("GetBanReason\n");
					MafiaNet::GetBanReason_Func func;
					GetUserName(&func);
					func.roomId=1;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'X':
				{
					printf("AddUserToQuickJoin\n");
					MafiaNet::AddUserToQuickJoin_Func func;
					GetUserName(&func);
					func.networkedQuickJoinUser.timeout=30000;
					func.networkedQuickJoinUser.minimumPlayers=4;
					func.gameIdentifier=GAME_IDENTIFIER;
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'Y':
				{
					printf("RemoveUserFromQuickJoin\n");
					MafiaNet::RemoveUserFromQuickJoin_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case 'Z':
				{
					printf("IsInQuickJoin\n");
					MafiaNet::IsInQuickJoin_Func func;
					GetUserName(&func);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case '1':
				{
					printf("SearchByFilter\n");
					MafiaNet::SearchByFilter_Func func;
					GetUserName(&func);
					func.gameIdentifier=GAME_IDENTIFIER;
					func.onlyJoinable=true;
					// Not specifying any search parameters returns all rooms
				//	MafiaNet::RakString roomName;
				//	GetRoomName(roomName);
				//	func.roomQuery.AddQuery_STRING(
				//		DefaultRoomColumns::GetColumnName(DefaultRoomColumns::TC_ROOM_NAME),
				//		roomName.C_String());
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case '2':
				{
					printf("ChangeHandle\n");
					MafiaNet::ChangeHandle_Func func;
					GetUserName(&func);
					func.newHandle="Crapola";
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case '3':
				{
					printf("RoomChat\n");
					MafiaNet::Chat_Func func;
					GetUserName(&func);
					func.chatMessage="Hello world. This is Crapola";
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			case '4':
				{
					printf("GetRoomProperties\n");
					MafiaNet::GetRoomProperties_Func func;
					GetUserName(&func);
					GetRoomName(func.roomName);
					roomsPluginClient.ExecuteFunc(&func);
				}
				break;
			}
		}
	}

	//MafiaNet::RakPeerInterface::DestroyInstance(client);
	//MafiaNet::RakPeerInterface::DestroyInstance(server);
}
