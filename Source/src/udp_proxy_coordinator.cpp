/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_UDPProxyCoordinator == 1 && _RAKNET_SUPPORT_UDPForwarder == 1

#include "mafianet/udp_proxy_coordinator.h"
#include "mafianet/bit_stream.h"
#include "mafianet/udp_proxy_common.h"
#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/rand.h"
#include "mafianet/get_time.h"
#include "mafianet/udp_forwarder.h"
#include "mafianet/vector_util.h"

#include <algorithm>
#include <string>

// Larger than the client version
static const int DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME = 2000;
static const int DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR = DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME + 1000;

using namespace MafiaNet;


int UDPProxyCoordinator::ServerWithPingComp(const unsigned short &key, const UDPProxyCoordinator::ServerWithPing &data)
{
	if (key < data.ping)
		return -1;
	if (key > data.ping)
		return 1;
	return 0;
}

int UDPProxyCoordinator::ForwardingRequestComp(const SenderAndTargetAddress &key, ForwardingRequest *const &data)
{
	if (key.senderClientAddress < data->sata.senderClientAddress)
		return -1;
	if (key.senderClientAddress > data->sata.senderClientAddress)
		return 1;
	if (key.targetClientAddress < data->sata.targetClientAddress)
		return -1;
	if (key.targetClientAddress > data->sata.targetClientAddress)
		return 1;
	return 0;
}

namespace
{
// Where \a sata is, or belongs, in the list kept sorted by ForwardingRequestComp.
unsigned int FindForwardingRequest(const std::vector<UDPProxyCoordinator::ForwardingRequest *> &list, const UDPProxyCoordinator::SenderAndTargetAddress &sata, int (*comp)(const UDPProxyCoordinator::SenderAndTargetAddress &, UDPProxyCoordinator::ForwardingRequest *const &), bool *exists)
{
	const auto it = std::lower_bound(list.begin(), list.end(), sata, [comp](UDPProxyCoordinator::ForwardingRequest *const &element, const UDPProxyCoordinator::SenderAndTargetAddress &key)
	                                 { return comp(key, element) > 0; });
	*exists = it != list.end() && comp(sata, *it) == 0;
	return static_cast<unsigned int>(it - list.begin());
}
} // namespace

STATIC_FACTORY_DEFINITIONS(UDPProxyCoordinator, UDPProxyCoordinator);

UDPProxyCoordinator::UDPProxyCoordinator()
{
}
UDPProxyCoordinator::~UDPProxyCoordinator()
{
	Clear();
}
void UDPProxyCoordinator::SetRemoteLoginPassword(const std::string &password)
{
	remoteLoginPassword = password;
}
void UDPProxyCoordinator::Update(void)
{
	unsigned int idx;
	MafiaNet::TimeMS curTime = MafiaNet::GetTimeMS();
	ForwardingRequest *fw;
	idx = 0;
	while (idx < forwardingRequestList.size())
	{
		fw = forwardingRequestList[idx];
		if (fw->timeRequestedPings != 0 &&
		    curTime > fw->timeRequestedPings + DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR)
		{
			fw->OrderRemainingServersToTry();
			fw->timeRequestedPings = 0;
			TryNextServer(fw->sata, fw);
			idx++;
		}
		else if (fw->timeoutAfterSuccess != 0 &&
		         curTime > fw->timeoutAfterSuccess)
		{
			// Forwarding request succeeded, we waited a bit to prevent duplicates. Can forget about the entry now.
			delete fw;
			forwardingRequestList.erase(forwardingRequestList.begin() + idx);
		}
		else
			idx++;
	}
}
PluginReceiveResult UDPProxyCoordinator::OnReceive(Packet *packet)
{
	if (packet->data[0] == ID_UDP_PROXY_GENERAL && packet->length > 1)
	{
		switch (packet->data[1])
		{
		case ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR:
			OnForwardingRequestFromClientToCoordinator(packet);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case ID_UDP_PROXY_LOGIN_REQUEST_FROM_SERVER_TO_COORDINATOR:
			OnLoginRequestFromServerToCoordinator(packet);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case ID_UDP_PROXY_FORWARDING_REPLY_FROM_SERVER_TO_COORDINATOR:
			OnForwardingReplyFromServerToCoordinator(packet);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		case ID_UDP_PROXY_PING_SERVERS_REPLY_FROM_CLIENT_TO_COORDINATOR:
			OnPingServersReplyFromClientToCoordinator(packet);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
	}
	return RR_CONTINUE_PROCESSING;
}
void UDPProxyCoordinator::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) rakNetGUID;

	unsigned int idx, idx2;

	idx = 0;
	while (idx < forwardingRequestList.size())
	{
		if (forwardingRequestList[idx]->requestingAddress == systemAddress)
		{
			// Guy disconnected before the attempt completed
			delete forwardingRequestList[idx];
			forwardingRequestList.erase(forwardingRequestList.begin() + idx);
		}
		else
			idx++;
	}

	const auto server = std::find(serverList.begin(), serverList.end(), systemAddress);
	if (server != serverList.end())
	{
		// Remove dead server first, so TryNextServer below skips it
		SwapRemove(serverList, static_cast<std::size_t>(server - serverList.begin()));

		ForwardingRequest *fw;
		// For each pending client for this server, choose from remaining servers.
		for (idx2 = 0; idx2 < forwardingRequestList.size(); idx2++)
		{
			fw = forwardingRequestList[idx2];
			if (fw->currentlyAttemptedServerAddress == systemAddress)
			{
				// Try the next server
				TryNextServer(fw->sata, fw);
			}
		}
	}
}
void UDPProxyCoordinator::OnForwardingRequestFromClientToCoordinator(Packet *packet)
{
	MafiaNet::BitStream incomingBs(packet->data, packet->length, false);
	incomingBs.IgnoreBytes(2);
	SystemAddress sourceAddress;
	incomingBs.Read(sourceAddress);
	if (sourceAddress == UNASSIGNED_SYSTEM_ADDRESS)
		sourceAddress = packet->systemAddress;
	SystemAddress targetAddress;
	RakNetGUID targetGuid;
	bool usesAddress = false;
	incomingBs.Read(usesAddress);
	if (usesAddress)
	{
		incomingBs.Read(targetAddress);
		targetGuid = rakPeerInterface->GetGuidFromSystemAddress(targetAddress);
	}
	else
	{
		incomingBs.Read(targetGuid);
		targetAddress = rakPeerInterface->GetSystemAddressFromGuid(targetGuid);
	}
	ForwardingRequest *fw = new ForwardingRequest;
	fw->timeoutAfterSuccess = 0;
	incomingBs.Read(fw->timeoutOnNoDataMS);
	bool hasServerSelectionBitstream = false;
	incomingBs.Read(hasServerSelectionBitstream);
	if (hasServerSelectionBitstream)
		incomingBs.Read(&(fw->serverSelectionBitstream));

	MafiaNet::BitStream outgoingBs;
	SenderAndTargetAddress sata;
	sata.senderClientAddress = sourceAddress;
	sata.targetClientAddress = targetAddress;
	sata.targetClientGuid = targetGuid;
	sata.senderClientGuid = rakPeerInterface->GetGuidFromSystemAddress(sourceAddress);
	SenderAndTargetAddress sataReversed;
	sataReversed.senderClientAddress = targetAddress;
	sataReversed.targetClientAddress = sourceAddress;
	sataReversed.senderClientGuid = sata.targetClientGuid;
	sataReversed.targetClientGuid = sata.senderClientGuid;

	unsigned int insertionIndex;
	bool objectExists1, objectExists2;
	insertionIndex = FindForwardingRequest(forwardingRequestList, sata, ForwardingRequestComp, &objectExists1);
	FindForwardingRequest(forwardingRequestList, sataReversed, ForwardingRequestComp, &objectExists2);

	if (objectExists1 || objectExists2)
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_IN_PROGRESS);
		outgoingBs.Write(sata.senderClientAddress);
		outgoingBs.Write(targetAddress);
		outgoingBs.Write(targetGuid);
		// Request in progress, not completed
		unsigned short forwardingPort = 0;
		outgoingBs.Write(std::string{});
		outgoingBs.Write(forwardingPort);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
		delete fw;
		return;
	}

	if (serverList.empty())
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_NO_SERVERS_ONLINE);
		outgoingBs.Write(sata.senderClientAddress);
		outgoingBs.Write(targetAddress);
		outgoingBs.Write(targetGuid);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
		delete fw;
		return;
	}

	if (rakPeerInterface->GetConnectionState(targetAddress) != IS_CONNECTED && usesAddress == false)
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR);
		outgoingBs.Write(sata.senderClientAddress);
		outgoingBs.Write(targetAddress);
		outgoingBs.Write(targetGuid);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
		delete fw;
		return;
	}

	fw->sata = sata;
	fw->requestingAddress = packet->systemAddress;

	if (serverList.size() > 1)
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT);
		outgoingBs.Write(sourceAddress);
		outgoingBs.Write(targetAddress);
		outgoingBs.Write(targetGuid);
		unsigned short serverListSize = (unsigned short) serverList.size();
		outgoingBs.Write(serverListSize);
		for (unsigned short idx = 0; idx < serverListSize; idx++)
			outgoingBs.Write(serverList[idx]);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, sourceAddress, false);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, targetAddress, false);
		fw->timeRequestedPings = MafiaNet::GetTimeMS();
		fw->remainingServersToTry.assign(serverList.begin(), serverList.end());
		forwardingRequestList.insert(forwardingRequestList.begin() + insertionIndex, fw);
	}
	else
	{
		fw->timeRequestedPings = 0;
		fw->currentlyAttemptedServerAddress = serverList[0];
		forwardingRequestList.insert(forwardingRequestList.begin() + insertionIndex, fw);
		SendForwardingRequest(sourceAddress, targetAddress, fw->currentlyAttemptedServerAddress, fw->timeoutOnNoDataMS);
	}
}

void UDPProxyCoordinator::SendForwardingRequest(SystemAddress sourceAddress, SystemAddress targetAddress, SystemAddress serverAddress, MafiaNet::TimeMS timeoutOnNoDataMS)
{
	MafiaNet::BitStream outgoingBs;
	// Send request to desired server
	outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
	outgoingBs.Write((MessageID) ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER);
	outgoingBs.Write(sourceAddress);
	outgoingBs.Write(targetAddress);
	outgoingBs.Write(timeoutOnNoDataMS);
	rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, serverAddress, false);
}
void UDPProxyCoordinator::OnLoginRequestFromServerToCoordinator(Packet *packet)
{
	MafiaNet::BitStream incomingBs(packet->data, packet->length, false);
	incomingBs.IgnoreBytes(2);
	std::string password;
	incomingBs.Read(password);
	MafiaNet::BitStream outgoingBs;

	if (remoteLoginPassword.empty())
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_NO_PASSWORD_SET_FROM_COORDINATOR_TO_SERVER);
		outgoingBs.Write(password);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
		return;
	}

	if (remoteLoginPassword != password)
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_WRONG_PASSWORD_FROM_COORDINATOR_TO_SERVER);
		outgoingBs.Write(password);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
		return;
	}

	if (std::find(serverList.begin(), serverList.end(), packet->systemAddress) != serverList.end())
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_ALREADY_LOGGED_IN_FROM_COORDINATOR_TO_SERVER);
		outgoingBs.Write(password);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
		return;
	}
	serverList.push_back(packet->systemAddress);
	outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
	outgoingBs.Write((MessageID) ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER);
	outgoingBs.Write(password);
	rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
}
void UDPProxyCoordinator::OnForwardingReplyFromServerToCoordinator(Packet *packet)
{
	MafiaNet::BitStream incomingBs(packet->data, packet->length, false);
	incomingBs.IgnoreBytes(2);
	SenderAndTargetAddress sata;
	incomingBs.Read(sata.senderClientAddress);
	incomingBs.Read(sata.targetClientAddress);
	bool objectExists;
	unsigned int index = FindForwardingRequest(forwardingRequestList, sata, ForwardingRequestComp, &objectExists);
	if (objectExists == false)
	{
		// The guy disconnected before the request finished
		return;
	}
	ForwardingRequest *fw = forwardingRequestList[index];
	sata.senderClientGuid = fw->sata.senderClientGuid;
	sata.targetClientGuid = fw->sata.targetClientGuid;

	std::string serverPublicIp;
	if (!incomingBs.Read(serverPublicIp))
		return;

	if (serverPublicIp.empty())
	{
		char serverIP[64];
		packet->systemAddress.ToString(false, serverIP, static_cast<size_t>(64));
		serverPublicIp = serverIP;
	}

	unsigned char c;
	unsigned short forwardingPort;
	if (!incomingBs.Read(c) || !incomingBs.Read(forwardingPort))
		return;
	const auto success = static_cast<UDPForwarderResult>(c);

	MafiaNet::BitStream outgoingBs;
	if (success == UDPFORWARDER_SUCCESS)
	{
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_FORWARDING_SUCCEEDED);
		outgoingBs.Write(sata.senderClientAddress);
		outgoingBs.Write(sata.targetClientAddress);
		outgoingBs.Write(sata.targetClientGuid);
		outgoingBs.Write(serverPublicIp);
		outgoingBs.Write(forwardingPort);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, fw->requestingAddress, false);

		outgoingBs.Reset();
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_FORWARDING_NOTIFICATION);
		outgoingBs.Write(sata.senderClientAddress);
		outgoingBs.Write(sata.targetClientAddress);
		outgoingBs.Write(sata.targetClientGuid);
		outgoingBs.Write(serverPublicIp);
		outgoingBs.Write(forwardingPort);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, sata.targetClientAddress, false);

		// 05/18/09 Keep the entry around for some time after success, so duplicates are reported if attempting forwarding from the target system before notification of success
		fw->timeoutAfterSuccess = MafiaNet::GetTimeMS() + fw->timeoutOnNoDataMS;
		// forwardingRequestList.RemoveAtIndex(index);
		// delete fw;

		return;
	}
	else if (success == UDPFORWARDER_NO_SOCKETS)
	{
		// Try next server
		TryNextServer(sata, fw);
	}
	else
	{
		RakAssert(success == UDPFORWARDER_FORWARDING_ALREADY_EXISTS);

		// Return in progress
		outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
		outgoingBs.Write((MessageID) ID_UDP_PROXY_IN_PROGRESS);
		outgoingBs.Write(sata.senderClientAddress);
		outgoingBs.Write(sata.targetClientAddress);
		outgoingBs.Write(sata.targetClientGuid);
		outgoingBs.Write(serverPublicIp);
		outgoingBs.Write(forwardingPort);
		rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, fw->requestingAddress, false);
		forwardingRequestList.erase(forwardingRequestList.begin() + index);
		delete fw;
	}
}
void UDPProxyCoordinator::OnPingServersReplyFromClientToCoordinator(Packet *packet)
{
	MafiaNet::BitStream incomingBs(packet->data, packet->length, false);
	incomingBs.IgnoreBytes(2);
	unsigned short serversToPingSize;
	SystemAddress serverAddress;
	SenderAndTargetAddress sata;
	incomingBs.Read(sata.senderClientAddress);
	incomingBs.Read(sata.targetClientAddress);
	bool objectExists;
	unsigned int index = FindForwardingRequest(forwardingRequestList, sata, ForwardingRequestComp, &objectExists);
	if (objectExists == false)
		return;
	unsigned short idx;
	ServerWithPing swp;
	ForwardingRequest *fw = forwardingRequestList[index];
	if (fw->timeRequestedPings == 0)
		return;

	incomingBs.Read(serversToPingSize);
	if (packet->systemAddress == sata.senderClientAddress)
	{
		for (idx = 0; idx < serversToPingSize; idx++)
		{
			incomingBs.Read(swp.serverAddress);
			incomingBs.Read(swp.ping);
			unsigned int index2;
			for (index2 = 0; index2 < fw->sourceServerPings.size(); index2++)
			{
				if (fw->sourceServerPings[index2].ping >= swp.ping)
					break;
			}
			fw->sourceServerPings.insert(fw->sourceServerPings.begin() + index2, swp);
		}
	}
	else
	{
		for (idx = 0; idx < serversToPingSize; idx++)
		{
			incomingBs.Read(swp.serverAddress);
			incomingBs.Read(swp.ping);

			unsigned int index2;
			for (index2 = 0; index2 < fw->targetServerPings.size(); index2++)
			{
				if (fw->targetServerPings[index2].ping >= swp.ping)
					break;
			}
			// (Sic: the target's pings go to sourceServerPings, at a position found in
			// targetServerPings. Kept as it was; clamped so the position stays in range.)
			if (index2 > fw->sourceServerPings.size())
				index2 = static_cast<unsigned int>(fw->sourceServerPings.size());
			fw->sourceServerPings.insert(fw->sourceServerPings.begin() + index2, swp);
		}
	}

	// Both systems have to give us pings to progress here. Otherwise will timeout in Update()
	if (!fw->sourceServerPings.empty() &&
	    !fw->targetServerPings.empty())
	{
		fw->OrderRemainingServersToTry();
		fw->timeRequestedPings = 0;
		TryNextServer(fw->sata, fw);
	}
}
void UDPProxyCoordinator::TryNextServer(SenderAndTargetAddress sata, ForwardingRequest *fw)
{
	bool pickedGoodServer = false;
	while (!fw->remainingServersToTry.empty())
	{
		fw->currentlyAttemptedServerAddress = fw->remainingServersToTry.front();
		fw->remainingServersToTry.pop_front();
		if (std::find(serverList.begin(), serverList.end(), fw->currentlyAttemptedServerAddress) != serverList.end())
		{
			pickedGoodServer = true;
			break;
		}
	}

	if (pickedGoodServer == false)
	{
		SendAllBusy(sata.senderClientAddress, sata.targetClientAddress, sata.targetClientGuid, fw->requestingAddress);
		bool objectExists;
		const unsigned int index = FindForwardingRequest(forwardingRequestList, sata, ForwardingRequestComp, &objectExists);
		if (objectExists)
			forwardingRequestList.erase(forwardingRequestList.begin() + index);
		delete fw;
		return;
	}

	SendForwardingRequest(sata.senderClientAddress, sata.targetClientAddress, fw->currentlyAttemptedServerAddress, fw->timeoutOnNoDataMS);
}
void UDPProxyCoordinator::SendAllBusy(SystemAddress senderClientAddress, SystemAddress targetClientAddress, RakNetGUID targetClientGuid, SystemAddress requestingAddress)
{
	MafiaNet::BitStream outgoingBs;
	outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
	outgoingBs.Write((MessageID) ID_UDP_PROXY_ALL_SERVERS_BUSY);
	outgoingBs.Write(senderClientAddress);
	outgoingBs.Write(targetClientAddress);
	outgoingBs.Write(targetClientGuid);
	rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, requestingAddress, false);
}
void UDPProxyCoordinator::Clear(void)
{
	serverList.clear();
	for (ForwardingRequest *fw : forwardingRequestList)
	{
		delete fw;
	}
	forwardingRequestList.clear();
}
void UDPProxyCoordinator::ForwardingRequest::OrderRemainingServersToTry(void)
{
	std::vector<UDPProxyCoordinator::ServerWithPing> swpList;

	if (sourceServerPings.empty() && targetServerPings.empty())
		return;

	size_t idx;
	UDPProxyCoordinator::ServerWithPing swp;
	for (idx = 0; idx < remainingServersToTry.size(); idx++)
	{
		swp.serverAddress = remainingServersToTry[idx];
		swp.ping = 0;
		// The ping lists come from clients and need not have one entry per server: a missing
		// entry counts as unresponsive (it was read out of range before).
		if (idx < sourceServerPings.size())
			swp.ping += (unsigned short) (sourceServerPings[idx].ping);
		else
			swp.ping += (unsigned short) (DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME);
		if (idx < targetServerPings.size())
			swp.ping += (unsigned short) (targetServerPings[idx].ping);
		else
			swp.ping += (unsigned short) (DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME);
		swpList.push_back(swp);
	}
	// Lowest total ping first (ServerWithPingComp order); equal pings keep server-list order.
	std::stable_sort(swpList.begin(), swpList.end(), [](const UDPProxyCoordinator::ServerWithPing &a, const UDPProxyCoordinator::ServerWithPing &b)
	                 { return ServerWithPingComp(a.ping, b) < 0; });
	remainingServersToTry.clear();
	for (const UDPProxyCoordinator::ServerWithPing &sorted : swpList)
	{
		remainingServersToTry.push_back(sorted.serverAddress);
	}
}

#endif // _RAKNET_SUPPORT_*
