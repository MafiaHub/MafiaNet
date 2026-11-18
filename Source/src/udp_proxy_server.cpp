/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_UDPProxyServer == 1 && _RAKNET_SUPPORT_UDPForwarder == 1

#include "mafianet/udp_proxy_server.h"
#include "mafianet/bit_stream.h"
#include "mafianet/udp_proxy_common.h"
#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(UDPProxyServer, UDPProxyServer);

UDPProxyServer::UDPProxyServer()
{
	resultHandler = 0;
	socketFamily = AF_INET;
}
UDPProxyServer::~UDPProxyServer()
{
}
void UDPProxyServer::SetSocketFamily(unsigned short _socketFamily)
{
	socketFamily = _socketFamily;
}
void UDPProxyServer::SetResultHandler(UDPProxyServerResultHandler *rh)
{
	resultHandler = rh;
}
bool UDPProxyServer::LoginToCoordinator(const std::string &password, SystemAddress coordinatorAddress)
{
	if (loggingInCoordinators.count(coordinatorAddress) != 0)
		return false;
	if (loggedInCoordinators.count(coordinatorAddress) != 0)
		return false;
	MafiaNet::BitStream outgoingBs;
	outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
	outgoingBs.Write((MessageID) ID_UDP_PROXY_LOGIN_REQUEST_FROM_SERVER_TO_COORDINATOR);
	outgoingBs.Write(password);
	rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, coordinatorAddress, false);
	loggingInCoordinators.insert(coordinatorAddress);
	return true;
}
void UDPProxyServer::SetServerPublicIP(const std::string &ip)
{
	serverPublicIp = ip;
}
void UDPProxyServer::Update(void)
{
}
PluginReceiveResult UDPProxyServer::OnReceive(Packet *packet)
{
	// Make sure incoming messages from from UDPProxyCoordinator

	if (packet->data[0] == ID_UDP_PROXY_GENERAL && packet->length > 1)
	{
		switch (packet->data[1])
		{
		case ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER:
			// Only from a coordinator this server logged in to. (This compared the insertion index with -1,
			// which never matched, so it accepted a forwarding request from any system.)
			if (loggedInCoordinators.count(packet->systemAddress) != 0)
			{
				OnForwardingRequestFromCoordinatorToServer(packet);
				return RR_STOP_PROCESSING_AND_DEALLOCATE;
			}
			break;
		case ID_UDP_PROXY_NO_PASSWORD_SET_FROM_COORDINATOR_TO_SERVER:
		case ID_UDP_PROXY_WRONG_PASSWORD_FROM_COORDINATOR_TO_SERVER:
		case ID_UDP_PROXY_ALREADY_LOGGED_IN_FROM_COORDINATOR_TO_SERVER:
		case ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER:
		{
			if (loggingInCoordinators.erase(packet->systemAddress) != 0)
			{

				MafiaNet::BitStream incomingBs(packet->data, packet->length, false);
				incomingBs.IgnoreBytes(2);
				std::string password;
				incomingBs.Read(password);
				switch (packet->data[1])
				{
				case ID_UDP_PROXY_NO_PASSWORD_SET_FROM_COORDINATOR_TO_SERVER:
					if (resultHandler)
						resultHandler->OnNoPasswordSet(password, this);
					break;
				case ID_UDP_PROXY_WRONG_PASSWORD_FROM_COORDINATOR_TO_SERVER:
					if (resultHandler)
						resultHandler->OnWrongPassword(password, this);
					break;
				case ID_UDP_PROXY_ALREADY_LOGGED_IN_FROM_COORDINATOR_TO_SERVER:
					if (resultHandler)
						resultHandler->OnAlreadyLoggedIn(password, this);
					break;
				case ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER:
					// RakAssert(loggedInCoordinators.GetIndexOf(packet->systemAddress)==(unsigned int)-1);
					loggedInCoordinators.insert(packet->systemAddress);
					if (resultHandler)
						resultHandler->OnLoginSuccess(password, this);
					break;
				}
			}


			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
		}
	}
	return RR_CONTINUE_PROCESSING;
}
void UDPProxyServer::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) rakNetGUID;

	loggingInCoordinators.erase(systemAddress);
	loggedInCoordinators.erase(systemAddress);
}
void UDPProxyServer::OnRakPeerStartup(void)
{
	udpForwarder.Startup();
}
void UDPProxyServer::OnRakPeerShutdown(void)
{
	udpForwarder.Shutdown();
	loggingInCoordinators.clear();
	loggedInCoordinators.clear();
}
void UDPProxyServer::OnAttach(void)
{
	if (rakPeerInterface->IsActive())
		OnRakPeerStartup();
}
void UDPProxyServer::OnDetach(void)
{
	OnRakPeerShutdown();
}
void UDPProxyServer::OnForwardingRequestFromCoordinatorToServer(Packet *packet)
{
	SystemAddress sourceAddress, targetAddress;
	MafiaNet::BitStream incomingBs(packet->data, packet->length, false);
	incomingBs.IgnoreBytes(2);
	incomingBs.Read(sourceAddress);
	incomingBs.Read(targetAddress);
	MafiaNet::TimeMS timeoutOnNoDataMS;
	incomingBs.Read(timeoutOnNoDataMS);
	RakAssert(timeoutOnNoDataMS > 0 && timeoutOnNoDataMS <= UDP_FORWARDER_MAXIMUM_TIMEOUT);

	unsigned short forwardingPort = 0;
	UDPForwarderResult success = udpForwarder.StartForwarding(sourceAddress, targetAddress, timeoutOnNoDataMS, 0, socketFamily, &forwardingPort, 0);
	MafiaNet::BitStream outgoingBs;
	outgoingBs.Write((MessageID) ID_UDP_PROXY_GENERAL);
	outgoingBs.Write((MessageID) ID_UDP_PROXY_FORWARDING_REPLY_FROM_SERVER_TO_COORDINATOR);
	outgoingBs.Write(sourceAddress);
	outgoingBs.Write(targetAddress);
	outgoingBs.Write(serverPublicIp);
	outgoingBs.Write((unsigned char) success);
	outgoingBs.Write(forwardingPort);
	rakPeerInterface->Send(&outgoingBs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, packet->systemAddress, false);
}

#endif // _RAKNET_SUPPORT_*
