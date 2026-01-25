/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2024, MafiaHub
 *
 *  This source code was modified by MafiaHub. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "MasterServer.h"
#include "MasterServerMessageIDs.h"
#include "mafianet/peerinterface.h"
#include "mafianet/BitStream.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/StringCompressor.h"
#include "mafianet/GetTime.h"
#include "mafianet/statistics.h"
#include "mafianet/PacketPriority.h"
using namespace MafiaNet;

// Uncomment this define for debugging printfs
#define _SHOW_MASTER_SERVER_PRINTF
#ifdef _SHOW_MASTER_SERVER_PRINTF
#include <cstdio>
#endif

MasterServer::MasterServer()
{
}

MasterServer::~MasterServer()
{
	ClearServerList();
}

void MasterServer::Update(RakPeerInterface *peer)
{
	unsigned serverIndex;
	Time time;
	// TODO - should have multiple listing security

	time = MafiaNet::GetTime();

	serverIndex=0;
	while (serverIndex < gameServerList.serverList.Size())
	{
		if (time >= gameServerList.serverList[serverIndex]->nextPingTime)
		{
			if (gameServerList.serverList[serverIndex]->failedPingResponses>=NUMBER_OF_MISSED_PINGS_TO_DROP)
			{
				#ifdef _SHOW_MASTER_SERVER_PRINTF
				printf("Deleting %s for lack of ping response.\n", (char*)gameServerList.serverList[serverIndex]->connectionIdentifier.ToString(false));
				#endif
				gameServerList.serverList[serverIndex]->Clear();
				delete gameServerList.serverList[serverIndex];
				gameServerList.serverList.RemoveAtIndex(serverIndex);
			}
			else
			{
				gameServerList.serverList[serverIndex]->nextPingTime = time + KEEP_ALIVE_PING_FREQUENCY;

				if (rakPeer->GetIndexFromSystemAddress(gameServerList.serverList[serverIndex]->connectionIdentifier)==-1)
				{
					rakPeer->Ping((char*)gameServerList.serverList[serverIndex]->connectionIdentifier.ToString(false),
						gameServerList.serverList[serverIndex]->connectionIdentifier.GetPort(), false);

					gameServerList.serverList[serverIndex]->failedPingResponses++;
#ifdef _SHOW_MASTER_SERVER_PRINTF
					printf("Pinging %s. Waiting on %i repl(ies) so far.\n", (char*)gameServerList.serverList[serverIndex]->connectionIdentifier.ToString(false),gameServerList.serverList[serverIndex]->failedPingResponses);
#endif
				}
				else
				{
#ifdef _SHOW_MASTER_SERVER_PRINTF
					printf("Not pinging %s since they are currently connected.\n", (char*)gameServerList.serverList[serverIndex]->connectionIdentifier.ToString(false));
#endif
				}
				serverIndex++;
			}
		}
		else
			serverIndex++;
	}
}

bool MasterServer::OnReceive(RakPeerInterface *peer, Packet *packet)
{

	RakNetStatistics *rss;
	Time connectionTime;
	Time time;
	unsigned serverIndex;
	time = MafiaNet::GetTime();

	// Quick and dirty flood attack security:
	// If a client has been connected for more than 5 seconds,
	// and has sent more than 1000 bytes per second on average then ban them
	rss=rakPeer->GetStatistics(packet->systemAddress);
	if (rss)
	{
		connectionTime=time-rss->connectionStartTime;
		if (connectionTime > FLOOD_ATTACK_CHECK_DELAY &&
			(float)(rss->runningTotal[ACTUAL_BYTES_RECEIVED]) / (float) connectionTime > FLOOD_ATTACK_BYTES_PER_MS)
		{
			rakPeer->CloseConnection(packet->systemAddress, true,0);
#ifdef _SHOW_MASTER_SERVER_PRINTF
			printf("%s banned for session due to for flood attack\n", (char*)packet->systemAddress.ToString(false));
#endif
			rakPeer->AddToBanList(packet->systemAddress.ToString(false));

			// Find all servers with this IP and kill them.
			serverIndex=0;
			while (serverIndex < gameServerList.serverList.Size())
			{
				if (gameServerList.serverList[serverIndex]->connectionIdentifier.address.addr4.sin_addr.s_addr==packet->systemAddress.address.addr4.sin_addr.s_addr)
				{
					delete gameServerList.serverList[serverIndex];
					gameServerList.serverList.RemoveAtIndex(serverIndex);
				}
				else
					serverIndex++;
			}
		}
	}

	switch(packet->data[0])
	{
	case ID_QUERY_MASTER_SERVER:
		HandleQuery(packet);
		return true;
	case ID_MASTER_SERVER_DELIST_SERVER:
		HandleDelistServer(packet);
		return true;
	case ID_MASTER_SERVER_SET_SERVER:
		HandleUpdateServer(packet);
		return true;
	case ID_PONG:
		HandlePong(packet);
		return false;
	case ID_RELAYED_CONNECTION_NOTIFICATION:
		HandleRelayedConnectionNotification(packet);
		return true;
	}

	return false; // Absorb packet
}

bool MasterServer::PropagateToGame(Packet *packet) const
{
	unsigned char packetIdentifier = packet->data[ 0 ];

	return packetIdentifier!=ID_QUERY_MASTER_SERVER &&
		packetIdentifier!=ID_MASTER_SERVER_DELIST_SERVER &&
		packetIdentifier!=ID_MASTER_SERVER_SET_SERVER &&
		packetIdentifier!=ID_RELAYED_CONNECTION_NOTIFICATION;
}

void MasterServer::HandleDelistServer(Packet *packet)
{
	SystemAddress serverSystemAddress;
	int existingServerIndex;
	BitStream bitStream(packet->data, packet->length, false);

	bitStream.IgnoreBits(sizeof(unsigned char)*8); // Ignore the packet type enum
	unsigned short port;
	bitStream.Read(port);
	serverSystemAddress.address.addr4.sin_addr.s_addr = packet->systemAddress.address.addr4.sin_addr.s_addr;
	serverSystemAddress.SetPortHostOrder(port);

	existingServerIndex=gameServerList.GetIndexBySystemAddress(serverSystemAddress);
	if (existingServerIndex>=0)
	{
		gameServerList.serverList[existingServerIndex]->Clear();
		delete gameServerList.serverList[existingServerIndex];
		gameServerList.serverList.RemoveAtIndex(existingServerIndex);
	}
	//else
		// Server does not already exist

	#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("%i servers on the list\n", gameServerList.serverList.Size());
	#endif
}

void MasterServer::HandleQuery(Packet *packet)
{
	DataStructures::List<GameServer*> serversWithKeysList;
	char ruleIdentifier[256];
	unsigned index, serverIndex;
	int key;
	bool queryAll;
	BitStream outputBitStream;
	BitStream compressedString(packet->data, packet->length, false);
	compressedString.IgnoreBits(8*sizeof(unsigned char));

	queryAll=true;

	while (compressedString.GetNumberOfUnreadBits()>0)
	{
		// Generate a list of the indices of the servers that have one or more of the specified keys.
		StringCompressor::Instance()->DecodeString(ruleIdentifier, 256, &compressedString);
		if (ruleIdentifier[0]==0)
			// If we fail to read the first string, queryAll remains true.
			break;

		queryAll=false;

		if (IsReservedRuleIdentifier(ruleIdentifier))
			continue;

		for (index=0; index < gameServerList.serverList.Size(); index++)
		{
			if (gameServerList.serverList[index]->connectionIdentifier==UNASSIGNED_SYSTEM_ADDRESS)
				continue;

			if (gameServerList.serverList[index]->FindKey(ruleIdentifier))
			{
				serverIndex=serversWithKeysList.GetIndexOf(gameServerList.serverList[index]);
				if (serverIndex==MAX_UNSIGNED_LONG)
				{
					gameServerList.serverList[index]->numberOfKeysFound=1;
					serversWithKeysList.Insert(gameServerList.serverList[index], _FILE_AND_LINE_);
				}
				else
				{
					serversWithKeysList[serverIndex]->numberOfKeysFound++;
				}
			}
		}
	}

	// Write the packet id
	if (queryAll)
		outputBitStream.Write((unsigned char) ID_MASTER_SERVER_SET_SERVER);
	else
		outputBitStream.Write((unsigned char) ID_MASTER_SERVER_UPDATE_SERVER);
	if (queryAll)
	{
		// Write the number of servers
		outputBitStream.WriteCompressed((unsigned short)gameServerList.serverList.Size());

		for (index=0; index < gameServerList.serverList.Size(); index++)
		{
			// Write the whole server
			SerializeServer(gameServerList.serverList[index], &outputBitStream);
		}
	}
	else
	{
		compressedString.ResetReadPointer();
		compressedString.IgnoreBits(8*sizeof(unsigned char));

		// Write the number of servers with requested keys
		outputBitStream.WriteCompressed((unsigned short)serversWithKeysList.Size());

		// For each server, write the header which consists of the IP/PORT.
		// Then go through the list of requested keys and write those
		for (index=0; index < serversWithKeysList.Size(); index++)
		{
			SerializeSystemAddress(&(serversWithKeysList[index]->connectionIdentifier), &outputBitStream);

			outputBitStream.WriteCompressed((unsigned short)serversWithKeysList[index]->numberOfKeysFound);
			while (compressedString.GetNumberOfUnreadBits()>0)
			{
				// Generate a list of the indices of the servers that have one or more of the specified keys.
				StringCompressor::Instance()->DecodeString(ruleIdentifier, 256, &compressedString);
				if (ruleIdentifier[0]==0)
					break;
				if (IsReservedRuleIdentifier(ruleIdentifier))
					continue;

				serversWithKeysList[index]->FindKey(ruleIdentifier);
				key=serversWithKeysList[index]->keyIndex;
				if (key>=0)
					SerializeRule(serversWithKeysList[index]->serverRules[key], &outputBitStream);
			}
		}
	}

	rakPeer->Send(&outputBitStream, MEDIUM_PRIORITY, RELIABLE, 0, packet->systemAddress, false);
}

void MasterServer::HandleUpdateServer(Packet *packet)
{
	GameServer *gameServer;
	bool newServerAdded;
	BitStream incomingBitStream(packet->data, packet->length, false);
	incomingBitStream.IgnoreBits(8*sizeof(unsigned char));

	gameServer = DeserializeServer(&incomingBitStream);
	gameServer->connectionIdentifier.address.addr4.sin_addr.s_addr = packet->systemAddress.address.addr4.sin_addr.s_addr;

	UpdateServerList(gameServer, true, &newServerAdded);

	if (newServerAdded)
	{
		#ifdef _SHOW_MASTER_SERVER_PRINTF
		printf("Server added. %i servers on the list\n", gameServerList.serverList.Size());
		#endif
		gameServer->originationId=packet->systemAddress;
	}
	#ifdef _SHOW_MASTER_SERVER_PRINTF
	else
		printf("Server updated. %i servers on the list\n", gameServerList.serverList.Size());
	#endif
}

void MasterServer::OnModifiedPacket(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Modified packet.\n");
#endif
}

void MasterServer::HandleRelayedConnectionNotification(Packet *packet)
{
	char str[22];
	unsigned short clientGamePort, serverGamePort;
	BitStream incomingBitStream(packet->data, packet->length, false);
	incomingBitStream.IgnoreBits(8*sizeof(unsigned char));
	incomingBitStream.Read(clientGamePort);
	incomingBitStream.Read(serverGamePort);
	if (!StringCompressor::Instance()->DecodeString(str, 22, &incomingBitStream))
		return;

	if (str[0]==0)
		return;

	BitStream outgoingBitStream;
	outgoingBitStream.Write((unsigned char)ID_RELAYED_CONNECTION_NOTIFICATION);
	// Assumes the game client is on the same computer as the master client
	outgoingBitStream.Write(packet->systemAddress.address.addr4.sin_addr.s_addr); // This is the public IP, which the sender doesn't necessarily know
	outgoingBitStream.Write(clientGamePort);

	SystemAddress targetID;
	targetID.SetBinaryAddress(str);
	targetID.SetPortHostOrder(serverGamePort);

	// Given the IP and port of the game system, give me the index into the game server list
	int serverIndex = gameServerList.GetIndexBySystemAddress(targetID);

	if (serverIndex>=0)
	{
		#ifdef _SHOW_MASTER_SERVER_PRINTF
		printf("ID_RELAYED_CONNECTION_NOTIFICATION sent to %s:%i from %s:%i\n", str, serverGamePort, packet->systemAddress.ToString(false), packet->systemAddress.GetPort());
		#endif
		rakPeer->Send(&outgoingBitStream, HIGH_PRIORITY, RELIABLE, 0, gameServerList.serverList[serverIndex]->originationId, false);
	}
	else
	{
#ifdef _SHOW_MASTER_SERVER_PRINTF
		printf("ID_RELAYED_CONNECTION_NOTIFICATION not sent to %s:%i from %s:%i.\nMaster server does not know about target system.\n", str, serverGamePort, packet->systemAddress.ToString(false), packet->systemAddress.GetPort());
#endif
	}


}
