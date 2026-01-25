/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2019, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/TelnetTransport.h"
#include "mafianet/ConsoleServer.h"
#include "mafianet/LogCommandParser.h"
#include "mafianet/PacketConsoleLogger.h"
#include "mafianet/peerinterface.h"
#include "mafianet/sleep.h"
#include <stdio.h>
#include "mafianet/Getche.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/Kbhit.h"

using namespace MafiaNet;

void main(void)
{
	printf("Shows how to connect telnet to read PacketLogger output from RakPeer.\n");

	RakPeerInterface *rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	TelnetTransport tt;
	ConsoleServer consoleServer;
	LogCommandParser lcp;
	PacketConsoleLogger pcl;
	pcl.SetLogCommandParser(&lcp);
	consoleServer.AddCommandParser(&lcp);
	consoleServer.SetTransportProvider(&tt, 23);
	rakPeer->AttachPlugin(&pcl);

	MafiaNet::SocketDescriptor sd(0,0);
	MafiaNet::StartupResult sr = rakPeer->Startup(32, &sd, 1);
	(void) sr;
	RakAssert(sr==RAKNET_STARTED);

	printf("Use telnet 127.0.0.1 23 to connect from the command window\n");
	printf("Use 'Turn Windows features on and off' with 'Telnet Client' if needed.\n");
	printf("Once telnet has connected, type 'Logger subscribe'\n");
	printf("Press any key in this window once you have done all this.\n");
	MafiaNet::Packet *packet;
	while (!_kbhit())
	{
		consoleServer.Update();
		RakSleep(30);
	}

	MafiaNet::ConnectionAttemptResult car = rakPeer->Connect("natpunch.slikesoft.com", 61111, 0, 0);
	(void) car;
	for(;;)
	{
		for (packet=rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet=rakPeer->Receive())
		{
		}

		consoleServer.Update();
		RakSleep(30);
	}	
}
