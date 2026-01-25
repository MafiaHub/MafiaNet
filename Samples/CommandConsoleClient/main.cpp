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

#include "mafianet/peerinterface.h"

#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/Kbhit.h"
#ifdef _WIN32
#include "mafianet/WindowsIncludes.h" // Sleep
#else
#include <unistd.h> // usleep
#include <strings.h>
#include "mafianet/linux_adapter.h"
#include "mafianet/LinuxStrings.h"
#include "mafianet/osx_adapter.h"

//linux doesn't have stricmp but strcasecmp is same functionality
#define stricmp strcasecmp
#endif

#include "mafianet/Gets.h"


int main()
{
	MafiaNet::Packet *packet;
	MafiaNet::RakPeerInterface *rakPeer;
	bool isConnected=false;
	rakPeer= MafiaNet::RakPeerInterface::GetInstance();
	char command[512];
	printf("This sample demonstrates connecting to the command console.\n");
	printf("using the RakNet transport protocol\n");
	printf("It's the equivalent of a secure telnet client\n");
	printf("See the 'CommandConsoleServer' project.\n");
	printf("Difficulty: Intermediate\n\n");

	printf("RakNet secure command console.\n");
	printf("Commands:\n");
	printf("/Connect\n");
	printf("/Disconnect\n");
	printf("/Quit\n");
	printf("Any other command goes to the remote console\n");
	int exitcode = 0;
	for(;;)
	{
		if (_kbhit())
		{
			Gets(command,sizeof(command));

			if (_stricmp(command, "/quit")==0)
			{
				printf("Goodbye.\n");
				rakPeer->Shutdown(500, 0);
				break;
			}
			else if (_stricmp(command, "/disconnect")==0)
			{
				if (isConnected)
				{
					rakPeer->Shutdown(500, 0);
					isConnected=false;
					printf("Disconnecting.\n");
				}
				else
				{
					printf("Not currently connected.\n");
				}
			}
			else if (_stricmp(command, "/connect")==0)
			{
				if (isConnected)
				{
					printf("Disconnect first.\n");
				}
				else
				{
					char ip[128];
					char remotePort[64];
					char password[512];
					char localPort[64];
					printf("Enter remote IP: ");
					do {
						Gets(ip, sizeof(ip));
					} while(ip[0]==0);
					printf("Enter remote port: ");
					do {
						Gets(remotePort,sizeof(remotePort));
					} while(remotePort[0]==0);
					const int intRemotePort = atoi(remotePort);
					if ((intRemotePort < 0) || (intRemotePort > std::numeric_limits<unsigned short>::max())) {
						printf("Specified remote port %d is outside valid bounds [0, %u]", intRemotePort, std::numeric_limits<unsigned short>::max());
						exitcode = 1;
						break;
					}
					printf("Enter local port (enter for 0): ");
					Gets(localPort,sizeof(localPort));
					if (localPort[0]==0)
					{
						strcpy_s(localPort, "0");
					}
					const int intLocalPort = atoi(localPort);
					if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
						printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
						exitcode = 2;
						break;
					}
					printf("Enter console password (enter for none): ");
					Gets(password,sizeof(password));
					MafiaNet::SocketDescriptor socketDescriptor(static_cast<unsigned short>(intLocalPort),0);
					if (rakPeer->Startup(1, &socketDescriptor, 1)== MafiaNet::RAKNET_STARTED)
					{
						int passwordLen;
						if (password[0])
							passwordLen=(int) strlen(password)+1;
						else
							passwordLen=0;
						if (rakPeer->Connect(ip, static_cast<unsigned short>(intRemotePort), password, passwordLen)== MafiaNet::CONNECTION_ATTEMPT_STARTED)
							printf("Connecting...\nNote: if the password is wrong the other system will ignore us.\n");
						else
						{
							printf("Connect call failed.\n");
							rakPeer->Shutdown(0, 0);
						}
					}
					else
						printf("Initialize call failed.\n");					
					
				}				
			}
			else
			{
				if (isConnected)
				{
					MafiaNet::BitStream str;
					str.Write((unsigned char) ID_TRANSPORT_STRING);
					str.Write(command, (int) strlen(command)+1);
					rakPeer->Send(&str, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
				}
				else
				{
					printf("You must be connected to send commands.\n");
				}
			}
		}

		packet = rakPeer->Receive();
		if (packet)
		{
			switch (packet->data[0])
			{
			case ID_DISCONNECTION_NOTIFICATION:
				printf("The server disconnected us.\n");
				isConnected=false;
				break;
			case ID_CONNECTION_BANNED:
				printf("We are banned from this server.\n");
				isConnected=false;
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				printf("Connection attempt failed.\nThe password was wrong or there is no responsive machine at that IP/port.\n");
				isConnected=false;
				break;
			case ID_NO_FREE_INCOMING_CONNECTIONS:
				printf("Server is full.\n");
				isConnected=false;
				break;
			case ID_CONNECTION_LOST:
				printf("We lost the connection.\n");
				isConnected=false;
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
				printf("Connection accepted.\n");
				isConnected=true;
				break;
			case ID_TRANSPORT_STRING:
				printf("%s", packet->data+1);
				break;
			}

			rakPeer->DeallocatePacket(packet);
		}

		// This sleep keeps RakNet responsive
#ifdef _WIN32
		Sleep(30);
#else
		usleep(30 * 1000);
#endif
	}

	return exitcode;
}
