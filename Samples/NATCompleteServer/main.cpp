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

#include "slikenet/peerinterface.h"
#include "slikenet/sleep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits> // used for std::numeric_limits
#include "slikenet/Kbhit.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/BitStream.h"
#include "slikenet/sleep.h"
#include "slikenet/UDPProxyServer.h"
#include "slikenet/UDPProxyCoordinator.h"
#include "slikenet/NatPunchthroughServer.h"
#include "slikenet/NatTypeDetectionServer.h"
#include "slikenet/SocketLayer.h"
#include "slikenet/Getche.h"
#include "slikenet/Gets.h"
#include "CloudServerHelper.h"
#include "slikenet/CloudClient.h"
#include "slikenet/statistics.h"
#include "slikenet/RelayPlugin.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

//#define VERBOSE_LOGGING

using namespace MafiaNet;

enum FeatureSupport
{
	SUPPORTED,
	UNSUPPORTED,
	QUERY
};

enum FeatureList
{
	NAT_TYPE_DETECTION_SERVER,
	NAT_PUNCHTHROUGH_SERVER,
	RELAY_PLUGIN,
	UDP_PROXY_COORDINATOR,
	UDP_PROXY_SERVER,
	CLOUD_SERVER,
	FEATURE_LIST_COUNT,
};

static unsigned short DEFAULT_RAKPEER_PORT=61111;

#define NatTypeDetectionServerFramework_Supported QUERY
#define NatPunchthroughServerFramework_Supported QUERY
#define RelayPlugin_Supported QUERY
#define UDPProxyCoordinatorFramework_Supported UNSUPPORTED
#define UDPProxyServerFramework_Supported UNSUPPORTED
#define CloudServerFramework_Supported QUERY

struct SampleFramework
{
	virtual const char * QueryName(void)=0;
	virtual const char * QueryRequirements(void)=0;
	virtual const char * QueryFunction(void)=0;
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)=0;
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)=0;
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)=0;

	FeatureSupport isSupported;
};

struct NatTypeDetectionServerFramework : public SampleFramework
{
	NatTypeDetectionServerFramework() {isSupported=NatTypeDetectionServerFramework_Supported; ntds=0;}
	virtual const char * QueryName(void) {return "NatTypeDetectionServer";}
	virtual const char * QueryRequirements(void) {return "Requires 4 IP addresses";}
	virtual const char * QueryFunction(void) {return "Determines router type to filter by connectable systems.\nOne instance needed, multiple instances may exist to spread workload.";}
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (isSupported==SUPPORTED)
		{
			ntds = new NatTypeDetectionServer;
			rakPeer->AttachPlugin(ntds);

			if (rakPeer->GetNumberOfAddresses() < 4)
			{
					printf("Failed. Not enough IP addresses to bind to.\n");
					rakPeer->DetachPlugin(ntds);
					delete ntds;
					ntds=0;
					isSupported=UNSUPPORTED;
					return;
			}
			ntds->Startup(rakPeer->GetLocalIP(1), rakPeer->GetLocalIP(2), rakPeer->GetLocalIP(3));
		}
	}
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)
	{
		// unused parameters
		(void)rakPeer;
		(void)packet;
	}
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (ntds)
		{
			rakPeer->DetachPlugin(ntds);
			delete ntds;
		}
		ntds=0;
	}

	NatTypeDetectionServer *ntds;
};
struct NatPunchthroughServerFramework : public SampleFramework, public NatPunchthroughServerDebugInterface_Printf
{
	NatPunchthroughServerFramework() {isSupported=NatPunchthroughServerFramework_Supported; nps=0;}
	virtual const char * QueryName(void) {return "NatPunchthroughServerFramework";}
	virtual const char * QueryRequirements(void) {return "None";}
	virtual const char * QueryFunction(void) {return "Coordinates NATPunchthroughClient.";}
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (isSupported==SUPPORTED)
		{
			nps = new NatPunchthroughServer;
			rakPeer->AttachPlugin(nps);
			#ifdef VERBOSE_LOGGING
				nps->SetDebugInterface(this);
			#endif
		}
	}
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)
	{
		// unused parameters
		(void)rakPeer;
		(void)packet;
	}
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (nps)
		{
			rakPeer->DetachPlugin(nps);
			delete nps;
		}
		nps=0;
	}

	NatPunchthroughServer *nps;
};
struct RelayPluginFramework : public SampleFramework
{
	RelayPluginFramework() {relayPlugin=nullptr;isSupported=RelayPlugin_Supported;}
	virtual const char * QueryName(void) {return "RelayPlugin";}
	virtual const char * QueryRequirements(void) {return "None.";}
	virtual const char * QueryFunction(void) {return "Relays messages between named connections.";}
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (isSupported==SUPPORTED)
		{
			relayPlugin = new RelayPlugin;
			rakPeer->AttachPlugin(relayPlugin);
			relayPlugin->SetAcceptAddParticipantRequests(true);
		}
	}
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)
	{
		// unused parameters
		(void)rakPeer;
		(void)packet;
	}
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (relayPlugin)
		{
			rakPeer->DetachPlugin(relayPlugin);
			delete relayPlugin;
			relayPlugin=0;
		}
	}

	RelayPlugin *relayPlugin;
};
struct UDPProxyCoordinatorFramework : public SampleFramework
{
	UDPProxyCoordinatorFramework() {udppc=0; isSupported=UDPProxyCoordinatorFramework_Supported;}
	virtual const char * QueryName(void) {return "UDPProxyCoordinator";}
	virtual const char * QueryRequirements(void) {return "Bandwidth to handle a few hundred bytes per game session.";}
	virtual const char * QueryFunction(void) {return "Coordinates UDPProxyClient to find available UDPProxyServer.\nExactly one instance required.";}
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (isSupported==SUPPORTED)
		{
			udppc = new UDPProxyCoordinator;
			rakPeer->AttachPlugin(udppc);

			char password[512];
			printf("Create password for UDPProxyCoordinator: ");
			Gets(password,sizeof(password));
			if (password[0]==0)
			{
				password[0]='a';
				password[1]=0;
			}
			udppc->SetRemoteLoginPassword(password);
		}
	}
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)
	{
		// unused parameters
		(void)rakPeer;
		(void)packet;
	}
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (udppc)
		{
			rakPeer->DetachPlugin(udppc);
			delete udppc;
			udppc=0;
		}
	}

	UDPProxyCoordinator *udppc;
};
SystemAddress SelectAmongConnectedSystems(MafiaNet::RakPeerInterface *rakPeer, const char *hostName)
{
	DataStructures::List<SystemAddress> addresses;
	DataStructures::List<RakNetGUID> guids;
	rakPeer->GetSystemList(addresses, guids);
	if (addresses.Size()==0)
	{
		return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	}
	if (addresses.Size()>1)
	{
		printf("Select IP address for %s.\n", hostName);
		char buff[64];
		for (unsigned int i=0; i < addresses.Size(); i++)
		{
			addresses[i].ToString(true, buff, static_cast<size_t>(64));
			printf("%i. %s\n", i+1, buff);
		}
		Gets(buff,sizeof(buff));
		if (buff[0]==0)
		{
			return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
		}
		unsigned int idx = atoi(buff);
		if (idx<=0 || idx > addresses.Size())
		{
			return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
		}
		return addresses[idx-1];
	}
	else
		return addresses[0];
};
SystemAddress ConnectBlocking(MafiaNet::RakPeerInterface *rakPeer, const char *hostName)
{
	SystemAddress returnvalue = MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	char ipAddr[64];
	printf("Enter IP of system %s is running on: ", hostName);
	Gets(ipAddr,sizeof(ipAddr));
	if (ipAddr[0]==0)
	{
		printf("Failed. Not connected to %s.\n", hostName);
		return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	}
	char port[64];
	printf("Enter port of system %s is running on: ", hostName);
	Gets(port, sizeof(port));
	if (port[0]==0)
	{
		printf("Failed. Not connected to %s.\n", hostName);
		return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	}
	const int intPort = atoi(port);
	if ((intPort < 0) || (intPort > std::numeric_limits<unsigned short>::max())) {
		printf("Failed. Specified port %d is outside valid bounds [0, %u]", intPort, std::numeric_limits<unsigned short>::max());
		return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	}
	if (rakPeer->Connect(ipAddr, static_cast<unsigned short>(intPort), 0, 0)!= MafiaNet::CONNECTION_ATTEMPT_STARTED)
	{
		printf("Failed connect call for %s.\n", hostName);
		return MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	}
	printf("Connecting...\n");
	MafiaNet::Packet *packet;
	// #med - review --- at least we'd add a sleep interval here - also review whether the behavior is correct to only check the very first received packet (old RakNet code was bogus in this regards)
	do {
		packet = rakPeer->Receive();
	} while (packet == nullptr);

	if (packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED)
		returnvalue = packet->systemAddress;

	rakPeer->DeallocatePacket(packet);

	return returnvalue;
}
struct UDPProxyServerFramework : public SampleFramework, public UDPProxyServerResultHandler
{
	UDPProxyServerFramework() {udpps=0; isSupported=UDPProxyServerFramework_Supported;}
	virtual const char * QueryName(void) {return "UDPProxyServer";}
	virtual const char * QueryRequirements(void) {return "Bandwidth to handle forwarded game traffic.";}
	virtual const char * QueryFunction(void) {return "Allows game clients to forward network traffic transparently.\nOne or more instances required, can be added at runtime.";}
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (isSupported==SUPPORTED)
		{
			printf("Logging into UDPProxyCoordinator...\n");
			SystemAddress coordinatorAddress=SelectAmongConnectedSystems(rakPeer, "UDPProxyCoordinator");
			if (coordinatorAddress== MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			{
				printf("Warning: RakPeer is not currently connected to any system.\nEnter option:\n(1). UDPProxyCoordinator is on localhost\n(2). Connect to a remote system\n(3). Fail.\nOption: ");
				int ch=_getche();
				printf("\n");
				if (ch=='1' || ch==13) // 13 is just pressing return
				{
					coordinatorAddress=rakPeer->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS,0);
				}
				else if (ch=='2')
				{
					coordinatorAddress=ConnectBlocking(rakPeer, "UDPProxyCoordinator");
					if (coordinatorAddress== MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
					{
						printf("Failed to connect.\n");
						isSupported=QUERY;
						return;
					}
				}
				else
				{
					printf("Failed. Not connected to UDPProxyCoordinator.\n");
					isSupported=QUERY;
					return;
				}
			}
			
			char password[512];
			printf("Enter password used with UDPProxyCoordinator: ");
			Gets(password,sizeof(password));
			if (password[0]==0)
			{
				password[0]='a';
				password[1]=0;
			}

			udpps = new UDPProxyServer;
			udpps->SetResultHandler(this);
			rakPeer->AttachPlugin(udpps);
			if (udpps->LoginToCoordinator(password, coordinatorAddress)==false)
			{
				printf("LoginToCoordinator call failed.\n");
				isSupported=QUERY;
				rakPeer->DetachPlugin(udpps);
				delete udpps;
				udpps=0;
				return;
			}
		}
	}
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)
	{
		// unused parameters
		(void)rakPeer;
		(void)packet;
	}
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (udpps)
		{
			rakPeer->DetachPlugin(udpps);
			delete udpps;
			udpps=0;
		}
	}

	virtual void OnLoginSuccess(MafiaNet::RakString usedPassword, MafiaNet::UDPProxyServer *proxyServerPlugin)
	{
		// unused parameters
		(void)proxyServerPlugin;

		printf("%s logged into UDPProxyCoordinator.\n", QueryName());
	}
	virtual void OnAlreadyLoggedIn(MafiaNet::RakString usedPassword, MafiaNet::UDPProxyServer *proxyServerPlugin)
	{
		// unused parameters
		(void)proxyServerPlugin;

		printf("%s already logged into UDPProxyCoordinator.\n", QueryName());
	}
	virtual void OnNoPasswordSet(MafiaNet::RakString usedPassword, MafiaNet::UDPProxyServer *proxyServerPlugin)
	{
		// unused parameters
		(void)proxyServerPlugin;

		printf("%s failed login to UDPProxyCoordinator. No password set.\n", QueryName());
		isSupported=QUERY;
		delete udpps;
		udpps=0;
	}
	virtual void OnWrongPassword(MafiaNet::RakString usedPassword, MafiaNet::UDPProxyServer *proxyServerPlugin)
	{
		// unused parameters
		(void)proxyServerPlugin;

		printf("%s failed login to UDPProxyCoordinator. %s was the wrong password.\n", QueryName(), usedPassword.C_String());
		isSupported=QUERY;
		delete udpps;
		udpps=0;
	}

	UDPProxyServer *udpps;
};
struct CloudServerFramework : public SampleFramework
{
	CloudServerFramework() {cloudServer=nullptr;isSupported=CloudServerFramework_Supported;}
	virtual const char * QueryName(void) {return "CloudServer";}
	virtual const char * QueryRequirements(void) {return "None.";}
	virtual const char * QueryFunction(void) {return "Single instance cloud server that maintains connection counts\nUseful as a directory server to find other client instances.";}
	virtual void Init(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (isSupported==SUPPORTED)
		{
			cloudServer = new CloudServer;
			rakPeer->AttachPlugin(cloudServer);
			cloudClient = new CloudClient;
			rakPeer->AttachPlugin(cloudClient);
			cloudServerHelperFilter = new CloudServerHelperFilter;
			cloudServer->AddQueryFilter(cloudServerHelperFilter);
			cloudServer->SetMaxUploadBytesPerClient(65535);
			cloudServerHelper.OnConnectionCountChange(rakPeer, cloudClient);
		}
	}
	virtual void ProcessPacket(MafiaNet::RakPeerInterface *rakPeer, Packet *packet)
	{
		if (isSupported!=SUPPORTED)
			return;

		switch (packet->data[0])
		{
		case ID_NEW_INCOMING_CONNECTION:
#ifdef VERBOSE_LOGGING
			printf("Got connection to %s\n", packet->systemAddress.ToString(true));
#endif
			cloudServerHelper.OnConnectionCountChange(rakPeer, cloudClient);
			break;
		case ID_CONNECTION_LOST:
		case ID_DISCONNECTION_NOTIFICATION:
#ifdef VERBOSE_LOGGING
			printf("Lost connection to %s\n", packet->systemAddress.ToString(true));
#endif
			cloudServerHelper.OnConnectionCountChange(rakPeer, cloudClient);
			break;
		}
	}
	virtual void Shutdown(MafiaNet::RakPeerInterface *rakPeer)
	{
		if (cloudServer)
		{
			rakPeer->DetachPlugin(cloudServer);
			delete cloudServer;
			cloudServer=0;
			rakPeer->DetachPlugin(cloudClient);
			delete cloudClient;
			cloudClient=0;
			delete cloudServerHelperFilter;
			cloudServerHelperFilter=0;
		}
	}

	MafiaNet::CloudServer *cloudServer;
	MafiaNet::CloudClient *cloudClient;
	MafiaNet::CloudServerHelperFilter *cloudServerHelperFilter;
	MafiaNet::CloudServerHelper cloudServerHelper;
};
int main(int argc, char **argv)
{
	MafiaNet::RakPeerInterface *rakPeer= MafiaNet::RakPeerInterface::GetInstance();
	SystemAddress ipList[ MAXIMUM_NUMBER_OF_INTERNAL_IDS ];
	printf("IPs:\n");
	unsigned int i;
	for (i=0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++) {
		ipList[i]=rakPeer->GetLocalIP(i);
		if (ipList[i]!=UNASSIGNED_SYSTEM_ADDRESS)
			printf("%i. %s\n", i+1, ipList[i].ToString(false));
		else
			break;
	}

	if (i == 0 && argc <= 3) {
		printf("Could not determine any local IP address.\n");
		return 3;
	}

	// If RakPeer is started on 2 IP addresses, NATPunchthroughServer supports port stride detection, improving success rate
	int sdLen=1;
	MafiaNet::SocketDescriptor sd[2];
	if (argc>1)
	{
		const int intPeerPort = atoi(argv[1]);
		if ((intPeerPort < 0) || (intPeerPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified peer port %d is outside valid bounds [0, %u]", intPeerPort, std::numeric_limits<unsigned short>::max());
			return 2;
		}
		DEFAULT_RAKPEER_PORT = static_cast<unsigned short>(intPeerPort);
	}

	// set the first IP address
	sd[0].port = DEFAULT_RAKPEER_PORT;
	// #med - improve the logic here to simplify the handling...
	if (argc > 2)
		strcpy_s(sd[0].hostAddress, argv[2]);

	// #high - improve determining the proper IP addresses
	//         - filter between IPv4/IPv6 and only use either of these
	//         - fallback to other IP addresses, if a given one failed to be bound
	// allow enforcing single IP address mode by specifying second/third argument to the same IP address
	if ((i >= 2 && argc <= 3) || (argc > 3)) {
		const char *ipAddress1 = (argc > 2) ? argv[2] : ipList[0].ToString(false);
		const char *ipAddress2 = (argc > 3) ? argv[3] : ipList[1].ToString(false);
		strcpy_s(sd[0].hostAddress, ipAddress1);
		sd[1].port = DEFAULT_RAKPEER_PORT+1;
		strcpy_s(sd[1].hostAddress, ipAddress2);
		printf("Dual IP address mode.\nFirst IP Address: '%s' (port: %u)\nSecond IP Address: '%s' (port: %u)\n", ipAddress1, sd[0].port, ipAddress2, sd[1].port);
		sdLen = 2;
	}
	else {
		printf("Single IP address mode.\nUsing port %i\n", sd[0].port);
	}

	const StartupResult success = rakPeer->Startup(8096, sd, sdLen);
	if (success != MafiaNet::RAKNET_STARTED)
	{
		printf("Failed to start rakPeer! Quitting - error code: %d\n", success);
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
		return 1;
	}
	rakPeer->SetTimeoutTime(5000, UNASSIGNED_SYSTEM_ADDRESS);
	printf("Started on %s\n\n", rakPeer->GetMyBoundAddress().ToString(true));

	rakPeer->SetMaximumIncomingConnections(8096);

	SampleFramework *samples[FEATURE_LIST_COUNT];
	i=0;
	samples[i++] = new NatTypeDetectionServerFramework;
	samples[i++] = new NatPunchthroughServerFramework;
	samples[i++] = new RelayPluginFramework;
	samples[i++] = new UDPProxyCoordinatorFramework;
	samples[i++] = new UDPProxyServerFramework;
	samples[i++] = new CloudServerFramework;
	assert(i==FEATURE_LIST_COUNT);

	bool isFirstPrint=true;
	for (i=0; i < FEATURE_LIST_COUNT; i++)
	{
		if (samples[i]->isSupported==QUERY)
		{
			if (isFirstPrint)
			{
				printf("NAT traversal server.\nSee http://www.dx.net/raknet_dx.php for discounted server hosting\nSelect which features to support.\n");
				isFirstPrint=false;
			}
			printf("\n%s\nRequirements: %s\nDescription: %s\n", samples[i]->QueryName(), samples[i]->QueryRequirements(), samples[i]->QueryFunction());
			printf("Support %s? (y/n): ", samples[i]->QueryName());
			int supported=_getche();
			if (supported=='y' || supported=='Y' || supported==13) // 13 is just pressing return
			{
				samples[i]->isSupported=SUPPORTED;
			}
			printf("\n");
		}
	}

	printf("\n");

	for (i=0; i < FEATURE_LIST_COUNT; i++)
	{
		if (samples[i]->isSupported==SUPPORTED)
		{
			printf("Starting %s...\n", samples[i]->QueryName());
			samples[i]->Init(rakPeer);
			if (samples[i]->isSupported!=SUPPORTED)
			{
				printf("Failed to start %s.", samples[i]->QueryName());
				if (samples[i]->isSupported==QUERY)
				{
					printf(" Retry? (y/n): ");
					int supported=_getche();
					if (supported=='y' || supported=='Y')
					{
						samples[i]->isSupported=SUPPORTED;
						i--;
					}
				}
				else
					printf("\n");
				printf("\n");
			}
			else
				printf("Success.\n\n");
		}
	}

	bool anySupported=false;
	for (i=0; i < FEATURE_LIST_COUNT; i++)
	{
		if (samples[i]->isSupported==SUPPORTED)
		{
			anySupported=true;
			break;
		}
	}

	if (anySupported==false)
	{
		printf("No features supported! Quitting.\n");
		rakPeer->Shutdown(100);
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
		return 1;
	}

	bool firstComma=true;
	printf("Supported features: ");
	for (i=0; i < FEATURE_LIST_COUNT; i++)
	{
		if (samples[i]->isSupported==SUPPORTED)
		{
			if (firstComma==false)
				printf(", ");
			else
				firstComma=false;
			printf("%s", samples[i]->QueryName());
		}
	}
	
	printf("\nEntering update loop. Press 'q' to quit.\n");

	MafiaNet::Packet *packet;
	bool quit=false;
	while (!quit)
	{
		for (packet=rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet=rakPeer->Receive())
		{
			for (i=0; i < FEATURE_LIST_COUNT; i++)
			{
				samples[i]->ProcessPacket(rakPeer, packet);
			}
		}

		if (_kbhit())
		{
			int ch = _getch();
			if (ch=='q')
			{
				quit=true;
			}
			else if (ch==' ')
			{
				RakNetStatistics rns;
				char message[2048];
				bool hasStatistics = rakPeer->GetStatistics(0, &rns);
				if (hasStatistics)
				{
					StatisticsToString(&rns, message, 2048, 2);
					printf("SYSTEM 0:\n%s\n", message);

					memset(&rns, 0, sizeof(RakNetStatistics));
					rakPeer->GetStatistics(UNASSIGNED_SYSTEM_ADDRESS, &rns);
					StatisticsToString(&rns, message, 2048, 2);
					printf("STAT SUM:\n%s\n", message);
				}
				else
				{
					printf("No system 0\n");
				}

				DataStructures::List<SystemAddress> addresses;
				DataStructures::List<RakNetGUID> guids;
				rakPeer->GetSystemList(addresses, guids);
				printf("%i systems connected\n", addresses.Size());
			}
		}
		RakSleep(30);
	}

	printf("Quitting.\n");
	for (i=0; i < FEATURE_LIST_COUNT; i++)
	{
		samples[i]->Shutdown(rakPeer);
	}
	rakPeer->Shutdown(100);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
	return 0;
}

