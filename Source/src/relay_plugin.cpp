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

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_RelayPlugin == 1

#include "mafianet/relay_plugin.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/bit_stream.h"
#include "mafianet/vector_util.h"

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(RelayPlugin, RelayPlugin);

RelayPlugin::RelayPlugin()
{
	acceptAddParticipantRequests = false;
}

RelayPlugin::~RelayPlugin() = default;

RelayPlugin::StrAndGuidAndRoom **RelayPlugin::FindParticipant(const RakNetGUID &guid)
{
	const auto participant = guidToStrHash.find(guid);
	return participant == guidToStrHash.end() ? nullptr : &participant->second;
}

RelayPluginEnums RelayPlugin::AddParticipantOnServer(const std::string &key, const RakNetGUID &guid)
{
	ConnectionState cs = rakPeerInterface->GetConnectionState(guid);
	if (cs != IS_CONNECTED)
		return RPE_ADD_CLIENT_TARGET_NOT_CONNECTED;

	if (strToGuidHash.count(key) != 0)
		return RPE_ADD_CLIENT_NAME_ALREADY_IN_USE; // Name already in use

	// If GUID is already in use, remove existing
	const auto existing = guidToStrHash.find(guid);
	if (existing != guidToStrHash.end())
	{
		const std::string existingName = existing->second->str;
		guidToStrHash.erase(existing);
		strToGuidHash.erase(existingName);
	}

	std::unique_ptr<StrAndGuidAndRoom> strAndGuid(new StrAndGuidAndRoom);
	strAndGuid->guid = guid;
	strAndGuid->str = key;

	guidToStrHash[guid] = strAndGuid.get();
	strToGuidHash[key] = std::move(strAndGuid);

	return RPE_ADD_CLIENT_SUCCESS;
}
void RelayPlugin::RemoveParticipantOnServer(const RakNetGUID &guid)
{
	const auto participant = guidToStrHash.find(guid);
	if (participant != guidToStrHash.end())
	{
		StrAndGuidAndRoom *strAndGuid = participant->second;
		guidToStrHash.erase(participant);
		LeaveGroup(&strAndGuid);
		const std::string name = strAndGuid->str;
		strToGuidHash.erase(name); // frees strAndGuid
	}
}

void RelayPlugin::SetAcceptAddParticipantRequests(bool accept)
{
	acceptAddParticipantRequests = accept;
}
void RelayPlugin::AddParticipantRequestFromClient(const std::string &key, const RakNetGUID &relayPluginServerGuid)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_ADD_CLIENT_REQUEST_FROM_CLIENT);
	bsOut.WriteCompressed(key);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, relayPluginServerGuid, false);
}
void RelayPlugin::RemoveParticipantRequestFromClient(const RakNetGUID &relayPluginServerGuid)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_REMOVE_CLIENT_REQUEST_FROM_CLIENT);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, relayPluginServerGuid, false);
}
// Send a message to a server running RelayPlugin, to forward a message to the system identified by \a key
void RelayPlugin::SendToParticipant(const RakNetGUID &relayPluginServerGuid, const std::string &key, BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_MESSAGE_TO_SERVER_FROM_CLIENT);
	bsOut.WriteCasted<unsigned char>(priority);
	bsOut.WriteCasted<unsigned char>(reliability);
	bsOut.Write(orderingChannel);
	bsOut.WriteCompressed(key);
	bsOut.Write(bitStream);
	SendUnified(&bsOut, priority, reliability, orderingChannel, relayPluginServerGuid, false);
}
void RelayPlugin::SendGroupMessage(const RakNetGUID &relayPluginServerGuid, BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_GROUP_MESSAGE_FROM_CLIENT);
	bsOut.WriteCasted<unsigned char>(priority);
	bsOut.WriteCasted<unsigned char>(reliability);
	bsOut.Write(orderingChannel);
	bsOut.Write(bitStream);
	SendUnified(&bsOut, priority, reliability, orderingChannel, relayPluginServerGuid, false);
}
void RelayPlugin::LeaveGroup(const RakNetGUID &relayPluginServerGuid)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_LEAVE_GROUP_REQUEST_FROM_CLIENT);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, relayPluginServerGuid, false);
}
void RelayPlugin::GetGroupList(const RakNetGUID &relayPluginServerGuid)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_GET_GROUP_LIST_REQUEST_FROM_CLIENT);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, relayPluginServerGuid, false);
}
PluginReceiveResult RelayPlugin::OnReceive(Packet *packet)
{
	if (packet->data[0] == ID_RELAY_PLUGIN)
	{
		switch (packet->data[1])
		{
		case RPE_MESSAGE_TO_SERVER_FROM_CLIENT:
		{
			BitStream bsIn(packet->data, packet->length, false);
			bsIn.IgnoreBytes(sizeof(MessageID) * 2);
			MafiaNet::Priority priority;
			MafiaNet::Reliability reliability;
			char orderingChannel;
			unsigned char cIn;
			bsIn.Read(cIn);
			priority = (MafiaNet::Priority) cIn;
			bsIn.Read(cIn);
			reliability = (MafiaNet::Reliability) cIn;
			bsIn.Read(orderingChannel);
			std::string key;
			bsIn.ReadCompressed(key);
			BitStream bsData;
			bsIn.Read(&bsData);
			const auto strAndGuidEntry = strToGuidHash.find(key);
			StrAndGuidAndRoom *strAndGuid = strAndGuidEntry == strToGuidHash.end() ? nullptr : strAndGuidEntry->second.get();
			StrAndGuidAndRoom **strAndGuidSender = FindParticipant(packet->guid);
			if (strAndGuid && strAndGuidSender)
			{
				BitStream bsOut;
				bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
				bsOut.WriteCasted<MessageID>(RPE_MESSAGE_TO_CLIENT_FROM_SERVER);
				bsOut.WriteCompressed((*strAndGuidSender)->str);
				bsOut.AlignWriteToByteBoundary();
				bsOut.Write(bsData);
				SendUnified(&bsOut, priority, reliability, orderingChannel, strAndGuid->guid, false);
			}

			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}

		case RPE_ADD_CLIENT_REQUEST_FROM_CLIENT:
		{
			BitStream bsIn(packet->data, packet->length, false);
			bsIn.IgnoreBytes(sizeof(MessageID) * 2);
			std::string key;
			bsIn.ReadCompressed(key);
			BitStream bsOut;
			bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
			if (acceptAddParticipantRequests)
				bsOut.WriteCasted<MessageID>(AddParticipantOnServer(key, packet->guid));
			else
				bsOut.WriteCasted<MessageID>(RPE_ADD_CLIENT_NOT_ALLOWED);
			bsOut.WriteCompressed(key);
			SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);

			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
		case RPE_REMOVE_CLIENT_REQUEST_FROM_CLIENT:
		{
			RemoveParticipantOnServer(packet->guid);
		}
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case RPE_GROUP_MESSAGE_FROM_CLIENT:
		{
			OnGroupMessageFromClient(packet);
		}
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case RPE_JOIN_GROUP_REQUEST_FROM_CLIENT:
		{
			OnJoinGroupRequestFromClient(packet);
		}
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case RPE_LEAVE_GROUP_REQUEST_FROM_CLIENT:
		{
			OnLeaveGroupRequestFromClient(packet);
		}
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case RPE_GET_GROUP_LIST_REQUEST_FROM_CLIENT:
		{
			SendChatRoomsList(packet->guid);
		}
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
	}

	return RR_CONTINUE_PROCESSING;
}

void RelayPlugin::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) systemAddress;

	RemoveParticipantOnServer(rakNetGUID);
}

RelayPlugin::RP_Group *RelayPlugin::JoinGroup(RP_Group *room, StrAndGuidAndRoom **strAndGuidSender)
{
	if (strAndGuidSender == 0)
		return 0;

	NotifyUsersInRoom(room, RPE_USER_ENTERED_ROOM, (*strAndGuidSender)->str);
	StrAndGuid sag;
	sag.guid = (*strAndGuidSender)->guid;
	sag.str = (*strAndGuidSender)->str;

	room->usersInRoom.push_back(sag);
	(*strAndGuidSender)->currentRoom = room->roomName;

	return room;
}
void RelayPlugin::JoinGroupRequest(const RakNetGUID &relayPluginServerGuid, const std::string &groupName)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_JOIN_GROUP_REQUEST_FROM_CLIENT);
	bsOut.WriteCompressed(groupName);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, relayPluginServerGuid, false);
}
RelayPlugin::RP_Group *RelayPlugin::JoinGroup(RakNetGUID userGuid, const std::string &roomName)
{
	StrAndGuidAndRoom **strAndGuidSender = FindParticipant(userGuid);
	if (strAndGuidSender)
	{
		if (roomName.empty())
			return 0;

		if ((*strAndGuidSender)->currentRoom == roomName)
			return 0;

		if ((*strAndGuidSender)->currentRoom.empty() == false)
			LeaveGroup(strAndGuidSender);

		std::string userName = (*strAndGuidSender)->str;

		for (const std::unique_ptr<RP_Group> &room : chatRooms)
		{
			if (room->roomName == roomName)
			{
				// Join existing room
				return JoinGroup(room.get(), strAndGuidSender);
			}
		}

		// Create new room
		RP_Group *room = new RP_Group;
		room->roomName = roomName;
		chatRooms.emplace_back(room);
		return JoinGroup(room, strAndGuidSender);
	}

	return 0;
}
void RelayPlugin::LeaveGroup(StrAndGuidAndRoom **strAndGuidSender)
{
	if (strAndGuidSender == 0)
		return;

	std::string userName = (*strAndGuidSender)->str;
	for (size_t i = 0; i < chatRooms.size(); i++)
	{
		if (chatRooms[i]->roomName == (*strAndGuidSender)->currentRoom)
		{
			(*strAndGuidSender)->currentRoom.clear();

			RP_Group *room = chatRooms[i].get();
			for (size_t j = 0; j < room->usersInRoom.size(); j++)
			{
				if (room->usersInRoom[j].guid == (*strAndGuidSender)->guid)
				{
					SwapRemove(room->usersInRoom, j);

					if (room->usersInRoom.empty())
					{
						SwapRemove(chatRooms, i); // frees room
						return;
					}
				}
			}

			NotifyUsersInRoom(room, RPE_USER_LEFT_ROOM, userName);

			return;
		}
	}
}
void RelayPlugin::NotifyUsersInRoom(RP_Group *room, int msg, const std::string &message)
{
	for (size_t i = 0; i < room->usersInRoom.size(); i++)
	{
		BitStream bsOut;
		bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
		bsOut.WriteCasted<MessageID>(msg);
		bsOut.WriteCompressed(message);

		SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, room->usersInRoom[i].guid, false);
	}
}
void RelayPlugin::SendMessageToRoom(StrAndGuidAndRoom **strAndGuidSender, BitStream *message, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel)
{
	if ((*strAndGuidSender)->currentRoom.empty())
		return;

	for (size_t i = 0; i < chatRooms.size(); i++)
	{
		if (chatRooms[i]->roomName == (*strAndGuidSender)->currentRoom)
		{
			BitStream bsOut;
			bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
			bsOut.WriteCasted<MessageID>(RPE_GROUP_MSG_FROM_SERVER);
			message->ResetReadPointer();
			bsOut.WriteCompressed((*strAndGuidSender)->str);
			bsOut.AlignWriteToByteBoundary();
			bsOut.Write(message);

			RP_Group *room = chatRooms[i].get();
			for (size_t j = 0; j < room->usersInRoom.size(); j++)
			{
				if (room->usersInRoom[j].guid != (*strAndGuidSender)->guid)
					SendUnified(&bsOut, priority, reliability, orderingChannel, room->usersInRoom[j].guid, false);
			}

			break;
		}
	}
}
void RelayPlugin::SendChatRoomsList(RakNetGUID target)
{
	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	bsOut.WriteCasted<MessageID>(RPE_GET_GROUP_LIST_REPLY_FROM_SERVER);
	bsOut.WriteCasted<uint16_t>(chatRooms.size());
	for (const std::unique_ptr<RP_Group> &room : chatRooms)
	{
		bsOut.WriteCompressed(room->roomName);
		bsOut.WriteCasted<uint16_t>(room->usersInRoom.size());
	}
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, target, false);
}
void RelayPlugin::OnGroupMessageFromClient(Packet *packet)
{
	BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	MafiaNet::Priority priority;
	MafiaNet::Reliability reliability;
	char orderingChannel;
	unsigned char cIn;
	bsIn.Read(cIn);
	priority = (MafiaNet::Priority) cIn;
	bsIn.Read(cIn);
	reliability = (MafiaNet::Reliability) cIn;
	bsIn.Read(orderingChannel);
	BitStream bsData;
	bsIn.Read(&bsData);

	StrAndGuidAndRoom **strAndGuidSender = FindParticipant(packet->guid);
	if (strAndGuidSender)
	{
		// Relay with the priority, reliability and channel the sender asked for in
		// SendGroupMessage(), as RPE_MESSAGE_TO_SERVER_FROM_CLIENT already does. These were
		// read and then dropped, so every group message went out High/ReliableOrdered/0.
		SendMessageToRoom(strAndGuidSender, &bsData, priority, reliability, orderingChannel);
	}
}
void RelayPlugin::OnJoinGroupRequestFromClient(Packet *packet)
{
	BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);
	std::string groupName;
	bsIn.ReadCompressed(groupName);
	RelayPlugin::RP_Group *groupJoined = JoinGroup(packet->guid, groupName);

	BitStream bsOut;
	bsOut.WriteCasted<MessageID>(ID_RELAY_PLUGIN);
	if (groupJoined)
	{
		bsOut.WriteCasted<MessageID>(RPE_JOIN_GROUP_SUCCESS);
		bsOut.WriteCasted<uint16_t>(groupJoined->usersInRoom.size());
		for (const StrAndGuid &user : groupJoined->usersInRoom)
		{
			bsOut.WriteCompressed(user.str);
		}
	}
	else
	{
		bsOut.WriteCasted<MessageID>(RPE_JOIN_GROUP_FAILURE);
	}

	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet->guid, false);
}
void RelayPlugin::OnLeaveGroupRequestFromClient(Packet *packet)
{
	BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);
	StrAndGuidAndRoom **strAndGuidSender = FindParticipant(packet->guid);
	if (strAndGuidSender)
		LeaveGroup(strAndGuidSender);
}
#endif // _RAKNET_SUPPORT_*
