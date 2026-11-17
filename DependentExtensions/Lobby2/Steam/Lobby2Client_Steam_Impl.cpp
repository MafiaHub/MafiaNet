/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "Lobby2Client_Steam_Impl.h"
#include "Lobby2Message_Steam.h"
#include <stdlib.h>
#include "mafianet/native_types.h"
#include "mafianet/mtu_size.h"
#include "mafianet/vector_util.h"
#include <windows.h>

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(Lobby2Client_Steam, Lobby2Client_Steam_Impl)

Lobby2Client_Steam_Impl::Lobby2Client_Steam_Impl()
    : m_CallbackLobbyDataUpdated(nullptr, nullptr),
      m_CallbackPersonaStateChange(nullptr, nullptr),
      m_CallbackLobbyDataUpdate(nullptr, nullptr),
      m_CallbackChatDataUpdate(nullptr, nullptr),
      m_CallbackChatMessageUpdate(nullptr, nullptr),
      m_CallbackP2PSessionRequest(nullptr, nullptr),
      m_CallbackP2PSessionConnectFail(nullptr, nullptr)
{
	// Must recompile RakNet with MAXIMUM_MTU_SIZE set to 1200 in the preprocessor settings
	RakAssert(MAXIMUM_MTU_SIZE <= 1200);
	roomId = 0;

	m_CallbackLobbyDataUpdated.Register(this, &Lobby2Client_Steam_Impl::OnLobbyDataUpdatedCallback);
	m_CallbackPersonaStateChange.Register(this, &Lobby2Client_Steam_Impl::OnPersonaStateChange);
	m_CallbackLobbyDataUpdate.Register(this, &Lobby2Client_Steam_Impl::OnLobbyDataUpdate);
	m_CallbackChatDataUpdate.Register(this, &Lobby2Client_Steam_Impl::OnLobbyChatUpdate);
	m_CallbackChatMessageUpdate.Register(this, &Lobby2Client_Steam_Impl::OnLobbyChatMessage);
	m_CallbackP2PSessionRequest.Register(this, &Lobby2Client_Steam_Impl::OnP2PSessionRequest);
	m_CallbackP2PSessionConnectFail.Register(this, &Lobby2Client_Steam_Impl::OnP2PSessionConnectFail);
}
Lobby2Client_Steam_Impl::~Lobby2Client_Steam_Impl()
{
}
void Lobby2Client_Steam_Impl::SendMsg(Lobby2Message *msg)
{
	if (msg->ClientImpl(this))
	{
		for (unsigned long i = 0; i < callbacks.size(); i++)
		{
			if (msg->callbackId == (uint32_t) -1 || msg->callbackId == callbacks[i]->callbackId)
				msg->CallCallback(callbacks[i]);
		}
	}
	else
	{
		// Won't be deleted by the user's call to Deref.
		msg->resultCode = L2RC_PROCESSING;
		msg->AddRef();
		PushDeferredCallback(msg);
	}
}
void Lobby2Client_Steam_Impl::Update(void)
{
	SteamAPI_RunCallbacks();

	/*
	// sending data
	// must be a handle to a connected socket
	// data is all sent via UDP, and thus send sizes are limited to 1200 bytes; after this, many routers will start dropping packets
	// use the reliable flag with caution; although the resend rate is pretty aggressive,
	// it can still cause stalls in receiving data (like TCP)
	virtual bool SendDataOnSocket( SNetSocket_t hSocket, void *pubData, uint32 cubData, bool bReliable ) = 0;

	// receiving data
	// returns false if there is no data remaining
	// fills out *pcubMsgSize with the size of the next message, in bytes
	virtual bool IsDataAvailableOnSocket( SNetSocket_t hSocket, uint32 *pcubMsgSize ) = 0; 

	// fills in pubDest with the contents of the message
	// messages are always complete, of the same size as was sent (i.e. packetized, not streaming)
	// if *pcubMsgSize < cubDest, only partial data is written
	// returns false if no data is available
	virtual bool RetrieveDataFromSocket( SNetSocket_t hSocket, void *pubDest, uint32 cubDest, uint32 *pcubMsgSize ) = 0; 
	*/
}

void Lobby2Client_Steam_Impl::PushDeferredCallback(Lobby2Message *msg)
{
	deferredCallbacks.push_back(msg);
}
void Lobby2Client_Steam_Impl::CallCBWithResultCode(Lobby2Message *msg, Lobby2ResultCode rc)
{
	if (msg)
	{
		msg->resultCode = rc;
		for (unsigned long i = 0; i < callbacks.size(); i++)
		{
			if (msg->callbackId == (uint32_t) -1 || msg->callbackId == callbacks[i]->callbackId)
				msg->CallCallback(callbacks[i]);
		}
	}
}
void Lobby2Client_Steam_Impl::OnLobbyMatchListCallback(LobbyMatchList_t *pCallback, bool bIOFailure)
{
	(void) bIOFailure;

	uint32_t i;
	for (i = 0; i < deferredCallbacks.size(); i++)
	{
		// Get any instance of Console_SearchRooms
		if (deferredCallbacks[i]->GetID() == L2MID_Console_SearchRooms)
		{
			Console_SearchRooms_Steam *callbackResult = (Console_SearchRooms_Steam *) deferredCallbacks[i];
			//			iterate the returned lobbies with GetLobbyByIndex(), from values 0 to m_nLobbiesMatching-1
			// lobbies are returned in order of closeness to the user, so add them to the list in that order
			for (uint32 iLobby = 0; iLobby < pCallback->m_nLobbiesMatching; iLobby++)
			{
				CSteamID steamId = SteamMatchmaking()->GetLobbyByIndex(iLobby);
				callbackResult->roomIds.push_back(steamId.ConvertToUint64());
				MafiaNet::RakString s = SteamMatchmaking()->GetLobbyData(steamId, "name");
				callbackResult->roomNames.push_back(s);
			}

			CallCBWithResultCode(callbackResult, L2RC_SUCCESS);
			msgFactory->Dealloc(callbackResult);
			SwapRemove(deferredCallbacks, i);
			break;
		}
	}
}
void Lobby2Client_Steam_Impl::OnLobbyDataUpdatedCallback(LobbyDataUpdate_t *pCallback)
{
	uint32_t i;
	for (i = 0; i < deferredCallbacks.size(); i++)
	{
		if (deferredCallbacks[i]->GetID() == L2MID_Console_GetRoomDetails)
		{
			Console_GetRoomDetails_Steam *callbackResult = (Console_GetRoomDetails_Steam *) deferredCallbacks[i];
			if (callbackResult->roomId == pCallback->m_ulSteamIDLobby)
			{
				const char *pchLobbyName = SteamMatchmaking()->GetLobbyData(pCallback->m_ulSteamIDLobby, "name");
				if (pchLobbyName[0])
				{
					callbackResult->roomName = pchLobbyName;
				}
				if (pCallback->m_bSuccess)
					CallCBWithResultCode(callbackResult, L2RC_SUCCESS);
				else
					CallCBWithResultCode(callbackResult, L2RC_Console_GetRoomDetails_NO_ROOMS_FOUND);
				msgFactory->Dealloc(callbackResult);
				SwapRemove(deferredCallbacks, i);
				break;
			}
		}
	}

	Console_GetRoomDetails_Steam notification;
	const char *pchLobbyName = SteamMatchmaking()->GetLobbyData(pCallback->m_ulSteamIDLobby, "name");
	if (pchLobbyName[0])
	{
		notification.roomName = pchLobbyName;
	}
	notification.roomId = pCallback->m_ulSteamIDLobby;
	CallCBWithResultCode(&notification, L2RC_SUCCESS);
}
void Lobby2Client_Steam_Impl::OnLobbyCreated(LobbyCreated_t *pCallback, bool bIOFailure)
{
	(void) bIOFailure;

	uint32_t i;
	for (i = 0; i < deferredCallbacks.size(); i++)
	{
		if (deferredCallbacks[i]->GetID() == L2MID_Console_CreateRoom)
		{
			Console_CreateRoom_Steam *callbackResult = (Console_CreateRoom_Steam *) deferredCallbacks[i];
			callbackResult->roomId = pCallback->m_ulSteamIDLobby;
			SteamMatchmaking()->SetLobbyData(callbackResult->roomId, "name", callbackResult->roomName.C_String());
			roomId = pCallback->m_ulSteamIDLobby;

			printf("\nNumber of Steam Lobby Members:%i in Lobby Name:%s\n", SteamMatchmaking()->GetNumLobbyMembers(roomId), callbackResult->roomName.C_String());
			RoomMember roomMember;
			roomMember.steamIDRemote = SteamMatchmaking()->GetLobbyOwner(roomId).ConvertToUint64();
			roomMember.systemAddress.address.addr4.sin_addr.s_addr = nextFreeSystemAddress++;
			roomMember.systemAddress.SetPortHostOrder(STEAM_UNUSED_PORT);
			roomMembersByAddr.emplace(roomMember.systemAddress, roomMember);
			roomMembersById.emplace(roomMember.steamIDRemote, roomMember);

			callbackResult->extendedResultCode = pCallback->m_eResult;
			if (pCallback->m_eResult == k_EResultOK)
			{
				CallCBWithResultCode(callbackResult, L2RC_SUCCESS);
			}
			else
			{
				CallCBWithResultCode(callbackResult, L2RC_GENERAL_ERROR);
			}
			msgFactory->Dealloc(callbackResult);
			SwapRemove(deferredCallbacks, i);

			// Commented out: Do not send the notification for yourself
			// CallRoomCallbacks();
			break;
		}
	}
}
void Lobby2Client_Steam_Impl::OnLobbyJoined(LobbyEnter_t *pCallback, bool bIOFailure)
{
	(void) bIOFailure;

	uint32_t i;
	for (i = 0; i < deferredCallbacks.size(); i++)
	{
		if (deferredCallbacks[i]->GetID() == L2MID_Console_JoinRoom)
		{
			Console_JoinRoom_Steam *callbackResult = (Console_JoinRoom_Steam *) deferredCallbacks[i];

			if (pCallback->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess)
			{
				roomId = pCallback->m_ulSteamIDLobby;

				CallCBWithResultCode(callbackResult, L2RC_SUCCESS);

				// First push to prevent being notified of ourselves
				RoomMember roomMember;
				roomMember.steamIDRemote = SteamUser()->GetSteamID().ConvertToUint64();
				roomMember.systemAddress.address.addr4.sin_addr.s_addr = nextFreeSystemAddress++;
				roomMember.systemAddress.SetPortHostOrder(STEAM_UNUSED_PORT);
				roomMembersByAddr.emplace(roomMember.systemAddress, roomMember);
				roomMembersById.emplace(roomMember.steamIDRemote, roomMember);

				CallRoomCallbacks();

				// In case the asynch lobby update didn't get it fast enough
				uint64_t myId64 = SteamUser()->GetSteamID().ConvertToUint64();
				if (roomMembersById.count(myId64) == 0)
				{
					roomMember.steamIDRemote = SteamMatchmaking()->GetLobbyOwner(roomId).ConvertToUint64();
					roomMember.systemAddress.address.addr4.sin_addr.s_addr = nextFreeSystemAddress++;
					roomMember.systemAddress.SetPortHostOrder(STEAM_UNUSED_PORT);
					roomMembersByAddr.emplace(roomMember.systemAddress, roomMember);
					roomMembersById.emplace(roomMember.steamIDRemote, roomMember);
				}
			}
			else
			{
				CallCBWithResultCode(callbackResult, L2RC_Console_JoinRoom_NO_SUCH_ROOM);
			}

			msgFactory->Dealloc(callbackResult);
			SwapRemove(deferredCallbacks, i);
			break;
		}
	}
}
bool Lobby2Client_Steam_Impl::IsCommandRunning(Lobby2MessageID msgId)
{
	uint32_t i;
	for (i = 0; i < deferredCallbacks.size(); i++)
	{
		if (deferredCallbacks[i]->GetID() == msgId)
		{
			return true;
		}
	}
	return false;
}

void Lobby2Client_Steam_Impl::OnPersonaStateChange(PersonaStateChange_t *pCallback)
{
	// callbacks are broadcast to all listeners, so we'll get this for every friend who changes state
	// so make sure the user is in the lobby before acting
	if (!SteamFriends()->IsUserInSource(pCallback->m_ulSteamID, roomId))
		return;

	if ((pCallback->m_nChangeFlags & k_EPersonaChangeNameFirstSet) ||
	    (pCallback->m_nChangeFlags & k_EPersonaChangeName))
	{
		Notification_Friends_StatusChange_Steam notification;
		notification.friendId = pCallback->m_ulSteamID;
		const char *pchName = SteamFriends()->GetFriendPersonaName(notification.friendId);
		notification.friendNewName = pchName;
		CallCBWithResultCode(&notification, L2RC_SUCCESS);
	}
}
void Lobby2Client_Steam_Impl::OnLobbyDataUpdate(LobbyDataUpdate_t *pCallback)
{
	// callbacks are broadcast to all listeners, so we'll get this for every lobby we're requesting
	if (roomId != pCallback->m_ulSteamIDLobby)
		return;

	Notification_Console_UpdateRoomParameters_Steam notification;
	notification.roomId = roomId;
	notification.roomNewName = SteamMatchmaking()->GetLobbyData(roomId, "name");
	if (pCallback->m_bSuccess)
		CallCBWithResultCode(&notification, L2RC_SUCCESS);
	else
		CallCBWithResultCode(&notification, L2RC_Console_GetRoomDetails_NO_ROOMS_FOUND);
}
void Lobby2Client_Steam_Impl::OnLobbyChatUpdate(LobbyChatUpdate_t *pCallback)
{
	// callbacks are broadcast to all listeners, so we'll get this for every lobby we're requesting
	if (roomId != pCallback->m_ulSteamIDLobby)
		return;

	// Purpose: Handles users in the lobby joining or leaving ??????
	CallRoomCallbacks();
}
void Lobby2Client_Steam_Impl::OnLobbyChatMessage(LobbyChatMsg_t *pCallback)
{
	CSteamID speaker;
	EChatEntryType entryType;
	char data[2048];
	int cubData = sizeof(data);
	SteamMatchmaking()->GetLobbyChatEntry(roomId, pCallback->m_iChatID, &speaker, data, cubData, &entryType);
	if (entryType == k_EChatEntryTypeChatMsg)
	{
		Notification_Console_RoomChatMessage_Steam notification;
		notification.message = data;
		CallCBWithResultCode(&notification, L2RC_SUCCESS);
	}
}
void Lobby2Client_Steam_Impl::GetRoomMembers(std::set<uint64_t> &_roomMembers)
{
	_roomMembers.clear();
	int cLobbyMembers = SteamMatchmaking()->GetNumLobbyMembers(roomId);
	for (int i = 0; i < cLobbyMembers; i++)
	{
		CSteamID steamIDLobbyMember = SteamMatchmaking()->GetLobbyMemberByIndex(roomId, i);
		uint64_t memberid = steamIDLobbyMember.ConvertToUint64();
		_roomMembers.insert(memberid);
	}
}

const char *Lobby2Client_Steam_Impl::GetRoomMemberName(uint64_t memberId)
{
	return SteamFriends()->GetFriendPersonaName(memberId);
}

bool Lobby2Client_Steam_Impl::IsRoomOwner(const uint64_t cSteamID)
{
	if (SteamUser()->GetSteamID() == SteamMatchmaking()->GetLobbyOwner(cSteamID))
		return true;

	return false;
}

bool Lobby2Client_Steam_Impl::IsInRoom(void) const
{
	return roomMembersById.size() > 0;
}

void Lobby2Client_Steam_Impl::CallRoomCallbacks()
{
	std::set<uint64_t> currentMembers;
	GetRoomMembers(currentMembers);
	std::map<uint64_t, RoomMember> updatedRoomMembers;
	bool anyChanges = false;

	// Both are sorted by Steam ID: merge them to find who joined and who left.
	// Merge over a copy: NotifyNewMember/NotifyDroppedMember run user callbacks synchronously, and a callback
	// can change roomMembersById (for example Console_LeaveRoom_Steam -> NotifyLeaveRoom -> ClearRoom), which
	// would invalidate an iterator into it.
	const std::map<uint64_t, RoomMember> oldMembers = roomMembersById;
	auto currentMember = currentMembers.begin();
	auto oldMember = oldMembers.begin();
	while (currentMember != currentMembers.end() && oldMember != oldMembers.end())
	{
		if (*currentMember < oldMember->second.steamIDRemote)
		{
			RoomMember roomMember;
			roomMember.steamIDRemote = *currentMember;
			roomMember.systemAddress.address.addr4.sin_addr.s_addr = nextFreeSystemAddress++;
			roomMember.systemAddress.SetPortHostOrder(STEAM_UNUSED_PORT);
			updatedRoomMembers.emplace(roomMember.steamIDRemote, roomMember);

			anyChanges = true;

			// new member
			NotifyNewMember(*currentMember, roomMember.systemAddress);
			++currentMember;
		}
		else if (*currentMember > oldMember->second.steamIDRemote)
		{
			anyChanges = true;

			// dropped member
			NotifyDroppedMember(oldMember->second.steamIDRemote, oldMember->second.systemAddress);
			++oldMember;
		}
		else
		{
			updatedRoomMembers.emplace(oldMember->second.steamIDRemote, oldMember->second);

			++currentMember;
			++oldMember;
		}
	}

	while (oldMember != oldMembers.end())
	{
		anyChanges = true;

		// dropped member
		NotifyDroppedMember(oldMember->second.steamIDRemote, oldMember->second.systemAddress);

		++oldMember;
	}
	while (currentMember != currentMembers.end())
	{
		RoomMember roomMember;
		roomMember.steamIDRemote = *currentMember;
		roomMember.systemAddress.address.addr4.sin_addr.s_addr = nextFreeSystemAddress++;
		roomMember.systemAddress.SetPortHostOrder(STEAM_UNUSED_PORT);
		updatedRoomMembers.emplace(roomMember.steamIDRemote, roomMember);

		anyChanges = true;

		// new member
		NotifyNewMember(*currentMember, roomMember.systemAddress);

		++currentMember;
	}

	if (anyChanges)
	{
		roomMembersById = updatedRoomMembers;
		roomMembersByAddr.clear();
		for (const auto &member : roomMembersById)
			roomMembersByAddr.emplace(member.second.systemAddress, member.second);
	}
}
void Lobby2Client_Steam_Impl::NotifyNewMember(uint64_t memberId, SystemAddress remoteSystem)
{
	// const char *pchName = SteamFriends()->GetFriendPersonaName( memberId );

	Notification_Console_MemberJoinedRoom_Steam notification;
	notification.roomId = roomId;
	notification.srcMemberId = memberId;
	notification.memberName = SteamFriends()->GetFriendPersonaName(memberId);
	notification.remoteSystem = remoteSystem;


	CallCBWithResultCode(&notification, L2RC_SUCCESS);
}
void Lobby2Client_Steam_Impl::NotifyDroppedMember(uint64_t memberId, SystemAddress remoteSystem)
{
	/// const char *pchName = SteamFriends()->GetFriendPersonaName( memberId );

	Notification_Console_MemberLeftRoom_Steam notification;
	notification.roomId = roomId;
	notification.srcMemberId = memberId;
	notification.memberName = SteamFriends()->GetFriendPersonaName(memberId);
	notification.remoteSystem = remoteSystem;
	CallCBWithResultCode(&notification, L2RC_SUCCESS);

	auto member = roomMembersById.find(memberId);
	if (member != roomMembersById.end())
	{
		rakPeerInterface->CloseConnection(member->second.systemAddress, false);
		// Is this necessary?
		SteamNetworking()->CloseP2PSessionWithUser(memberId);
	}
}
void Lobby2Client_Steam_Impl::ClearRoom(void)
{
	roomId = 0;
	if (SteamNetworking())
	{
		for (const auto &member : roomMembersById)
		{
			SteamNetworking()->CloseP2PSessionWithUser(member.second.steamIDRemote);
		}
	}
	roomMembersById.clear();
	roomMembersByAddr.clear();
}
void Lobby2Client_Steam_Impl::OnP2PSessionRequest(P2PSessionRequest_t *pCallback)
{
	// we'll accept a connection from anyone
	SteamNetworking()->AcceptP2PSessionWithUser(pCallback->m_steamIDRemote);
}
void Lobby2Client_Steam_Impl::OnP2PSessionConnectFail(P2PSessionConnectFail_t *pCallback)
{
	(void) pCallback;

	// we've sent a packet to the user, but it never got through
	// we can just use the normal timeout
}
void Lobby2Client_Steam_Impl::NotifyLeaveRoom(void)
{
	ClearRoom();
}

int Lobby2Client_Steam_Impl::RakNetSendTo(const char *data, int length, const SystemAddress &systemAddress)
{
	auto member = roomMembersByAddr.find(systemAddress);
	if (member != roomMembersByAddr.end())
	{
		if (SteamNetworking()->SendP2PPacket(member->second.steamIDRemote, data, length, k_EP2PSendUnreliable))
			return length;
		else
			return 0;
	}
	else if (systemAddress.GetPort() != STEAM_UNUSED_PORT)
	{
		//	return SocketLayer::SendTo_PC(s,data,length,systemAddress);
		return -1;
	}
	return 0;
}

int Lobby2Client_Steam_Impl::RakNetRecvFrom(char dataOut[MAXIMUM_MTU_SIZE], SystemAddress *senderOut, bool calledFromMainThread)
{
	(void) calledFromMainThread;

	uint32 pcubMsgSize;
	if (SteamNetworking() && SteamNetworking()->IsP2PPacketAvailable(&pcubMsgSize))
	{
		CSteamID psteamIDRemote;
		if (SteamNetworking()->ReadP2PPacket(dataOut, MAXIMUM_MTU_SIZE, &pcubMsgSize, &psteamIDRemote))
		{
			uint64_t steamIDRemote64 = psteamIDRemote.ConvertToUint64();
			auto member = roomMembersById.find(steamIDRemote64);
			if (member != roomMembersById.end())
			{
				*senderOut = member->second.systemAddress;
			}
			return pcubMsgSize;
		}
	}
	return 0;
}

void Lobby2Client_Steam_Impl::OnRakPeerShutdown(void)
{
	ClearRoom();
}
void Lobby2Client_Steam_Impl::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) rakNetGUID;

	auto member = roomMembersByAddr.find(systemAddress);
	if (member != roomMembersByAddr.end())
	{
		uint64_t steamIDRemote = member->second.steamIDRemote;

		SteamNetworking()->CloseP2PSessionWithUser(steamIDRemote);
		roomMembersByAddr.erase(member);

		const std::size_t erased = roomMembersById.erase(steamIDRemote);
		RakAssert(erased == 1);
		(void) erased;
	}
}
void Lobby2Client_Steam_Impl::OnFailedConnectionAttempt(Packet *packet, PI2_FailedConnectionAttemptReason failedConnectionAttemptReason)
{
	(void) failedConnectionAttemptReason;

	auto member = roomMembersByAddr.find(packet->systemAddress);
	if (member != roomMembersByAddr.end())
	{
		uint64_t steamIDRemote = member->second.steamIDRemote;

		SteamNetworking()->CloseP2PSessionWithUser(steamIDRemote);
		roomMembersByAddr.erase(member);

		const std::size_t erased = roomMembersById.erase(steamIDRemote);
		RakAssert(erased == 1);
		(void) erased;
	}
}
void Lobby2Client_Steam_Impl::OnAttach(void)
{
	nextFreeSystemAddress = (uint32_t) rakPeerInterface->GetMyGUID().g;

	// If this asserts, call RakPeer::Startup() before attaching Lobby2Client_Steam
	std::vector<RakNetSocket2 *> sockets;
	rakPeerInterface->GetSockets(sockets);
	RakAssert(sockets.size());
	((RNS2_Windows *) sockets[0])->SetSocketLayerOverride(this);
}
void Lobby2Client_Steam_Impl::OnDetach(void)
{
	std::vector<RakNetSocket2 *> sockets;
	rakPeerInterface->GetSockets(sockets);
	RakAssert(sockets.size());
	((RNS2_Windows *) sockets[0])->SetSocketLayerOverride(nullptr);
}
