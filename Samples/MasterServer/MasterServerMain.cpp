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

// This is my own internal test program for the master server but serves as a good example.

#include "MasterCommon.h"
#include "MasterServer.h"
#include "mafianet/peerinterface.h"
#include <cstdio>
#include <cstring>
#ifdef WIN32
#include <conio.h>
#else
#include "../Unix/kbhit.h"
#endif

#ifdef _WIN32
#include <conio.h>
#include <windows.h> // Sleep
#else
#include "../Unix/kbhit.h"
#include <unistd.h> // usleep
#endif

using namespace MafiaNet;

#define READCHAR(arg) gets(arg); ch=arg[0];

int main(void)
{
	MasterServer masterServer;
	int serverListSize;
	const char *outputString;
	int outputInt;
	bool identiferFound;
	int index;
	char ch;
	Packet *p;

	RakPeerInterface *testGameMasterServer;

	testGameMasterServer = RakPeerInterface::GetInstance();
	SocketDescriptor sd(60000, 0);
	testGameMasterServer->Startup(10, &sd, 1);
	testGameMasterServer->SetMaximumIncomingConnections(8);
	testGameMasterServer->AttachPlugin(&masterServer);

	printf("This project shows how to use the master server.\n");
	printf("The master server is a plugin that maintains a list of games uplodated.\n");
	printf("by the master client.\n");
	printf("Difficulty: Beginner\n\n");

	printf("(p)rint\n(q)uit\n");
	char buff[256];

	while (1)
	{
		if (kbhit())
		{
			READCHAR(buff);
			if (ch=='q')
				break;
			else if (ch=='p')
			{
				serverListSize=masterServer.GetServerListSize();
				if (serverListSize==0)
				{
					printf("No servers in list\n");
				}
				else
				{
					for (index=0; index < serverListSize; index++)
					{
						printf("%i. ", index);
						outputString=masterServer.GetServerListRuleAsString(index, "IP", &identiferFound);
						if (identiferFound)
							printf("%s:", outputString);
						else
							printf("NO_IP:");
						outputInt=masterServer.GetServerListRuleAsInt(index, "Port", &identiferFound);
						if (identiferFound)
							printf("%i ", outputInt);
						else
							printf("NO_PORT ");
						outputString=masterServer.GetServerListRuleAsString(index, "Game type", &identiferFound);
						if (identiferFound)
							printf("%s ", outputString);
						else
							printf("NIL_GT ");
						outputString=masterServer.GetServerListRuleAsString(index, "Game name", &identiferFound);
						if (identiferFound)
							printf("%s ", outputString);
						else
							printf("NIL_GN ");
						outputInt=masterServer.GetServerListRuleAsInt(index, "Score", &identiferFound);
						if (identiferFound)
							printf("%i\n", outputInt);
						else
							printf("NO_SCORE\n");
					}
				}
			}
			ch=0;
		}

		p = testGameMasterServer->Receive();
		while (p)
		{
			// Ignore any game packets.  The master server plugin handles everything.
			testGameMasterServer->DeallocatePacket(p);
			// Call Receive every update to keep the plugin going
			p = testGameMasterServer->Receive();
		}


#ifdef _WIN32
		Sleep(30);
#else
		usleep(30 * 1000);
#endif
	}

	RakPeerInterface::DestroyInstance(testGameMasterServer);

	return 0;
}
