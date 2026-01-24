/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017-2019, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#ifndef __ROOMS_BROWSER_GFX3_RAKNET
#define __ROOMS_BROWSER_GFX3_RAKNET

#include "slikenet/WindowsIncludes.h"
#include "slikenet/types.h"
#include "Lobby2Message.h"
#include "slikenet/PluginInterface2.h"
#include "RoomsBrowserGFx3.h"
#include "RoomsPlugin.h"

namespace MafiaNet {

/// This is data that should be set with RakPeerInterface::SetOfflinePingResponse()
/// It should also be able to write this data to a LobbyServer instance
struct ServerAndRoomBrowserData
{
	unsigned short numPlayers;
	unsigned short maxPlayers;
	MafiaNet::RakString mapName;
	MafiaNet::RakString roomName;
	MafiaNet::RakNetGUID roomId;
	
	/// For the LAN browser, it expects the offline ping response to be packed using the format from this structure
	/// Therefore, to advertise that your server is available or updated, fill out the parameters in this structure, then call SetAsOfflinePingResponse()
	/// When your server is no longer available, set the offline ping response to null
	void SetAsOfflinePingResponse(MafiaNet::RakPeerInterface *rakPeer);

	/// Write to a RakNet table data structure, useful when creating the room or setting the room properties
	/// Writes everything EXCEPT the list of players
	void WriteToTable(DataStructures::Table *table);

	/// \internal
	void Serialize(MafiaNet::BitStream *bitStream, bool writeToBitstream);
};

// GFxPlayerTinyD3D9.cpp has an instance of this class, and callls the corresponding 3 function
// This keeps the patching code out of the GFx sample as much as possible
class RoomsBrowserGFx3_RakNet : public RoomsBrowserGFx3, public MafiaNet::Lobby2Callbacks, public PluginInterface2, MafiaNet::RoomsCallback
{
public:
	RoomsBrowserGFx3_RakNet();
	virtual ~RoomsBrowserGFx3_RakNet();
	virtual void Init(MafiaNet::Lobby2Client *_lobby2Client,
		MafiaNet::Lobby2MessageFactory *_messageFactory,
		RakPeerInterface *_rakPeer, 
		MafiaNet::RoomsPlugin *_roomsPlugin,
		MafiaNet::RakString _titleName,
		MafiaNet::RakString _titleSecretKey,
		MafiaNet::RakString _pathToXMLPropertyFile,
		unsigned short _lanServerPort,
		GPtr<FxDelegate> pDelegate,
		GPtr<GFxMovieView> pMovie);

	void Update(void);
	void Shutdown(void);

	// Update all callbacks from flash
	void                Accept(CallbackProcessor* cbreg);

	virtual const char *QueryPlatform(void) const {return "RakNet";}
	virtual void SaveProperty(const char *propertyId, const char *propertyValue);
	virtual void LoadProperty(const char *propertyId, MafiaNet::RakString &propertyOut);

	ACTIONSCRIPT_CALLABLE_HEADER(f2c_ConnectToServer);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Login);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_RegisterAccount);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_UpdateRoomsList);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_UpdateFriendsList);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_JoinByFilter);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_CreateRoom);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Friends_SendInvite);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Friends_Remove);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Friends_AcceptInvite);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Friends_RejectInvite);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Directed_Chat_Func);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Room_Chat_Func);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_Logoff);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_LeaveRoom);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_SendInvite);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_StartSpectating);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_StopSpectating);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_GrantModerator);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_SetReadyStatus);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_GetReadyStatus);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_SetRoomLockState);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_GetRoomLockState);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_AreAllMembersReady);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_KickMember);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_GetRoomProperties);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_StartGame);

	// Lobby2
	virtual void MessageResult(MafiaNet::Client_Login *message);
	virtual void MessageResult(MafiaNet::Client_Logoff *message);
	virtual void MessageResult(MafiaNet::Client_RegisterAccount *message);
	virtual void MessageResult(MafiaNet::Friends_GetFriends *message);
	virtual void MessageResult(MafiaNet::Friends_GetInvites *message);
	virtual void MessageResult(MafiaNet::Friends_SendInvite *message);
	virtual void MessageResult(MafiaNet::Friends_AcceptInvite *message);
	virtual void MessageResult(MafiaNet::Friends_RejectInvite *message);
	virtual void MessageResult(MafiaNet::Friends_Remove *message);
	virtual void MessageResult(MafiaNet::Notification_Friends_PresenceUpdate *message);
	virtual void MessageResult(MafiaNet::Notification_Friends_StatusChange *message);

	// PluginInterface2
	virtual void OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason );
	virtual void OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming);
	virtual void OnFailedConnectionAttempt(Packet *packet, PI2_FailedConnectionAttemptReason failedConnectionAttemptReason);
	virtual PluginReceiveResult OnReceive(Packet *packet);

	/// Rooms callbacks
	virtual void CreateRoom_Callback( const SystemAddress &senderAddress, MafiaNet::CreateRoom_Func *callResult);
	virtual void SearchByFilter_Callback( const SystemAddress &senderAddress, MafiaNet::SearchByFilter_Func *callResult);
	virtual void JoinByFilter_Callback( const SystemAddress &senderAddress, MafiaNet::JoinByFilter_Func *callResult);
	virtual void Chat_Callback( const SystemAddress &senderAddress, MafiaNet::Chat_Func *callResult);
	virtual void Chat_Callback( const SystemAddress &senderAddress, MafiaNet::Chat_Notification *notification);
	virtual void LeaveRoom_Callback( const SystemAddress &senderAddress, MafiaNet::LeaveRoom_Func *callResult);
	virtual void SendInvite_Callback( const SystemAddress &senderAddress, MafiaNet::SendInvite_Func *callResult);
	virtual void StartSpectating_Callback( const SystemAddress &senderAddress, MafiaNet::StartSpectating_Func *callResult);
	virtual void StopSpectating_Callback( const SystemAddress &senderAddress, MafiaNet::StopSpectating_Func *callResult);
	virtual void GrantModerator_Callback( const SystemAddress &senderAddress, MafiaNet::GrantModerator_Func *callResult);
	virtual void SetReadyStatus_Callback( const SystemAddress &senderAddress, MafiaNet::SetReadyStatus_Func *callResult);
	virtual void GetReadyStatus_Callback( const SystemAddress &senderAddress, MafiaNet::GetReadyStatus_Func *callResult);
	virtual void SetRoomLockState_Callback( const SystemAddress &senderAddress, MafiaNet::SetRoomLockState_Func *callResult);
	virtual void GetRoomLockState_Callback( const SystemAddress &senderAddress, MafiaNet::GetRoomLockState_Func *callResult);
	virtual void AreAllMembersReady_Callback( const SystemAddress &senderAddress, MafiaNet::AreAllMembersReady_Func *callResult);
	virtual void KickMember_Callback( const SystemAddress &senderAddress, MafiaNet::KickMember_Func *callResult);
	virtual void GetRoomProperties_Callback( const SystemAddress &senderAddress, MafiaNet::GetRoomProperties_Func *callResult);

	// Notifications due to other room members
	virtual void RoomMemberStartedSpectating_Callback( const SystemAddress &senderAddress, MafiaNet::RoomMemberStartedSpectating_Notification *notification);
	virtual void RoomMemberStoppedSpectating_Callback( const SystemAddress &senderAddress, MafiaNet::RoomMemberStoppedSpectating_Notification *notification);
	virtual void ModeratorChanged_Callback( const SystemAddress &senderAddress, MafiaNet::ModeratorChanged_Notification *notification);
	virtual void RoomMemberReadyStatusSet_Callback( const SystemAddress &senderAddress, MafiaNet::RoomMemberReadyStatusSet_Notification *notification);
	virtual void RoomLockStateSet_Callback( const SystemAddress &senderAddress, MafiaNet::RoomLockStateSet_Notification *notification);
	virtual void RoomMemberKicked_Callback( const SystemAddress &senderAddress, MafiaNet::RoomMemberKicked_Notification *notification);
	virtual void RoomMemberLeftRoom_Callback( const SystemAddress &senderAddress, MafiaNet::RoomMemberLeftRoom_Notification *notification);
	virtual void RoomMemberJoinedRoom_Callback( const SystemAddress &senderAddress, MafiaNet::RoomMemberJoinedRoom_Notification *notification);
	virtual void RoomInvitationSent_Callback( const SystemAddress &senderAddress, MafiaNet::RoomInvitationSent_Notification *notification);
	virtual void RoomInvitationWithdrawn_Callback( const SystemAddress &senderAddress, MafiaNet::RoomInvitationWithdrawn_Notification *notification);
	virtual void RoomDestroyedOnModeratorLeft_Callback( const SystemAddress &senderAddress, MafiaNet::RoomDestroyedOnModeratorLeft_Notification *notification);

	MafiaNet::Lobby2Client *lobby2Client;
	MafiaNet::Lobby2MessageFactory *msgFactory;
	MafiaNet::RakPeerInterface *rakPeer;
	MafiaNet::RakString titleName;
	MafiaNet::RakString titleSecretKey;
	MafiaNet::RakString pathToXMLPropertyFile;
	MafiaNet::RakString loginUsername;
	MafiaNet::RoomsPlugin *roomsPlugin;
	unsigned short lanServerPort;
};

} // namespace MafiaNet

#endif // __ROOMS_BROWSER_GFX3_RAKNET
