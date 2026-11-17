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
#if _RAKNET_SUPPORT_ConnectionGraph2 == 1

#include "mafianet/connection_graph2.h"
#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(ConnectionGraph2, ConnectionGraph2)

ConnectionGraph2::ConnectionGraph2()
{
	autoProcessNewConnections = true;
}
ConnectionGraph2::~ConnectionGraph2()
{
}
bool ConnectionGraph2::GetConnectionListForRemoteSystem(RakNetGUID remoteSystemGuid, SystemAddress *saOut, RakNetGUID *guidOut, unsigned int *outLength)
{
	if ((saOut == 0 && guidOut == 0) || outLength == 0 || *outLength == 0 || remoteSystemGuid == UNASSIGNED_RAKNET_GUID)
	{
		*outLength = 0;
		return false;
	}

	const auto system = remoteSystems.find(remoteSystemGuid);
	if (system == remoteSystems.end())
	{
		*outLength = 0;
		return false;
	}

	const std::map<RakNetGUID, SystemAddressAndGuid> &connections = system->second->remoteConnections;
	if (connections.size() < *outLength)
		*outLength = (unsigned int) connections.size();
	unsigned int written = 0;
	for (auto it = connections.begin(); written < *outLength; ++it, ++written)
	{
		if (guidOut)
			guidOut[written] = it->second.guid;
		if (saOut)
			saOut[written] = it->second.systemAddress;
	}
	return true;
}
bool ConnectionGraph2::ConnectionExists(RakNetGUID g1, RakNetGUID g2)
{
	if (g1 == g2)
		return false;

	const auto system = remoteSystems.find(g1);
	if (system == remoteSystems.end())
	{
		return false;
	}
	return system->second->remoteConnections.count(g2) != 0;
}
uint16_t ConnectionGraph2::GetPingBetweenSystems(RakNetGUID g1, RakNetGUID g2) const
{
	if (g1 == g2)
		return 0;

	if (g1 == rakPeerInterface->GetMyGUID())
		return (uint16_t) rakPeerInterface->GetAveragePing(g2);
	if (g2 == rakPeerInterface->GetMyGUID())
		return (uint16_t) rakPeerInterface->GetAveragePing(g1);

	const auto system = remoteSystems.find(g1);
	if (system == remoteSystems.end())
	{
		return (uint16_t) -1;
	}

	const auto connection = system->second->remoteConnections.find(g2);
	if (connection == system->second->remoteConnections.end())
	{
		return (uint16_t) -1;
	}
	return connection->second.sendersPingToThatSystem;
}

/// Returns the system with the lowest total ping among all its connections. This can be used as the 'best host' for a peer to peer session
RakNetGUID ConnectionGraph2::GetLowestAveragePingSystem(void) const
{
	float lowestPing = -1.0;
	const RemoteSystem *lowestPingSystem = nullptr;
	float thisAvePing = 0.0f;
	int ap, count = 0;

	for (const auto &entry : remoteSystems)
	{
		thisAvePing = 0.0f;

		ap = rakPeerInterface->GetAveragePing(entry.second->guid);
		if (ap != -1)
		{
			thisAvePing += (float) ap;
			count++;
		}
	}

	if (count > 0)
	{
		lowestPing = thisAvePing / count;
	}

	for (const auto &entry : remoteSystems)
	{
		thisAvePing = 0.0f;
		count = 0;

		const RemoteSystem *remoteSystem = entry.second.get();
		for (const auto &connection : remoteSystem->remoteConnections)
		{
			ap = connection.second.sendersPingToThatSystem;
			if (ap != -1)
			{
				thisAvePing += (float) ap;
				count++;
			}
		}

		if (count > 0 && (lowestPing == -1.0f || thisAvePing / count < lowestPing))
		{
			lowestPing = thisAvePing / count;
			lowestPingSystem = remoteSystem;
		}
	}

	if (lowestPingSystem == nullptr)
		return rakPeerInterface->GetMyGUID();
	return lowestPingSystem->guid;
}

void ConnectionGraph2::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	// Send notice to all existing connections
	MafiaNet::BitStream bs;
	if (lostConnectionReason == LCR_CONNECTION_LOST)
		bs.Write((MessageID) ID_REMOTE_CONNECTION_LOST);
	else
		bs.Write((MessageID) ID_REMOTE_DISCONNECTION_NOTIFICATION);
	bs.Write(systemAddress);
	bs.Write(rakNetGUID);
	SendUnified(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, true);

	remoteSystems.erase(rakNetGUID);
}
void ConnectionGraph2::SetAutoProcessNewConnections(bool b)
{
	autoProcessNewConnections = b;
}
bool ConnectionGraph2::GetAutoProcessNewConnections(void) const
{
	return autoProcessNewConnections;
}
void ConnectionGraph2::AddParticipant(const SystemAddress &systemAddress, RakNetGUID rakNetGUID)
{
	// Relay the new connection to other systems.
	MafiaNet::BitStream bs;
	bs.Write((MessageID) ID_REMOTE_NEW_INCOMING_CONNECTION);
	bs.Write((uint32_t) 1);
	bs.Write(systemAddress);
	bs.Write(rakNetGUID);
	bs.WriteCasted<uint16_t>(rakPeerInterface->GetAveragePing(rakNetGUID));
	SendUnified(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, true);

	// Send everyone to the new guy
	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	rakPeerInterface->GetSystemList(addresses, guids);
	bs.Reset();
	bs.Write((MessageID) ID_REMOTE_NEW_INCOMING_CONNECTION);
	BitSize_t writeOffset = bs.GetWriteOffset();
	bs.Write((uint32_t) addresses.size());

	unsigned int i;
	uint32_t count = 0;
	for (i = 0; i < addresses.size(); i++)
	{
		if (addresses[i] == systemAddress)
			continue;

		bs.Write(addresses[i]);
		bs.Write(guids[i]);
		bs.WriteCasted<uint16_t>(rakPeerInterface->GetAveragePing(guids[i]));
		count++;
	}

	if (count > 0)
	{
		BitSize_t writeOffset2 = bs.GetWriteOffset();
		bs.SetWriteOffset(writeOffset);
		bs.Write(count);
		bs.SetWriteOffset(writeOffset2);
		SendUnified(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, systemAddress, false);
	}

	std::unique_ptr<RemoteSystem> &remoteSystem = remoteSystems[rakNetGUID];
	if (remoteSystem == nullptr)
	{
		remoteSystem = std::make_unique<RemoteSystem>();
		remoteSystem->guid = rakNetGUID;
	}
}
void ConnectionGraph2::GetParticipantList(std::set<RakNetGUID> &participantList)
{
	participantList.clear();
	for (const auto &entry : remoteSystems)
		participantList.insert(participantList.end(), entry.first);
}
void ConnectionGraph2::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
	(void) isIncoming;
	if (autoProcessNewConnections)
		AddParticipant(systemAddress, rakNetGUID);
}
PluginReceiveResult ConnectionGraph2::OnReceive(Packet *packet)
{
	if (packet->data[0] == ID_REMOTE_CONNECTION_LOST || packet->data[0] == ID_REMOTE_DISCONNECTION_NOTIFICATION)
	{
		const auto system = remoteSystems.find(packet->guid);
		if (system != remoteSystems.end())
		{
			MafiaNet::BitStream bs(packet->data, packet->length, false);
			bs.IgnoreBytes(1);
			SystemAddressAndGuid saag;
			bs.Read(saag.systemAddress);
			bs.Read(saag.guid);
			system->second->remoteConnections.erase(saag.guid);
		}
	}
	else if (packet->data[0] == ID_REMOTE_NEW_INCOMING_CONNECTION)
	{
		const auto system = remoteSystems.find(packet->guid);
		if (system != remoteSystems.end())
		{
			uint32_t numAddresses = 0;
			MafiaNet::BitStream bs(packet->data, packet->length, false);
			bs.IgnoreBytes(1);
			bs.Read(numAddresses);
			for (unsigned int idx2 = 0; idx2 < numAddresses; idx2++)
			{
				SystemAddressAndGuid saag;
				// A truncated list ends the loop instead of inserting unread entries.
				if (bs.Read(saag.systemAddress) == false || bs.Read(saag.guid) == false || bs.Read(saag.sendersPingToThatSystem) == false)
					break;
				// emplace keeps the first entry for a guid, as the sorted list did.
				system->second->remoteConnections.emplace(saag.guid, saag);
			}
		}
	}

	return RR_CONTINUE_PROCESSING;
}

#endif // _RAKNET_SUPPORT_*
