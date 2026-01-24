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

#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include "slikenet/GetTime.h"
#include "slikenet/Rand.h"
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/BitStream.h"
#include "slikenet/TwoWayAuthentication.h"
#include "slikenet/sleep.h"

static const int NUM_PEERS=2;
MafiaNet::RakPeerInterface *rakPeer[NUM_PEERS];
MafiaNet::TwoWayAuthentication *twoWayAuthenticationPlugin[NUM_PEERS];
int main(void)
{
	for (int i=0; i < NUM_PEERS; i++)
		rakPeer[i]= MafiaNet::RakPeerInterface::GetInstance();

	printf("This project tests and demonstrates the two way authentication plugin.\n");
	printf("Difficulty: Beginner\n\n");

	// Initialize the message handlers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		twoWayAuthenticationPlugin[peerIndex]= MafiaNet::OP_NEW<MafiaNet::TwoWayAuthentication>(_FILE_AND_LINE_);
		rakPeer[peerIndex]->AttachPlugin(twoWayAuthenticationPlugin[peerIndex]);
		rakPeer[peerIndex]->SetMaximumIncomingConnections(NUM_PEERS);
	}

	// Initialize the peers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		MafiaNet::SocketDescriptor socketDescriptor(60000+peerIndex,0);
		rakPeer[peerIndex]->Startup(NUM_PEERS, &socketDescriptor, 1);
	}
	
	// Connect each peer to the prior peer
	for (unsigned short peerIndex=1; peerIndex < NUM_PEERS; peerIndex++)
	{
        rakPeer[peerIndex]->Connect("127.0.0.1", 60000+peerIndex-1, 0, 0);
	}

	RakSleep(100);


	printf("Peers initialized and connected.\n");
	twoWayAuthenticationPlugin[0]->AddPassword("PWD0", "Password0");
	twoWayAuthenticationPlugin[0]->AddPassword("PWD1", "Password1");
	twoWayAuthenticationPlugin[1]->AddPassword("PWD0", "Password0");
	SLNET_VERIFY(twoWayAuthenticationPlugin[0]->Challenge("PWD0", rakPeer[0]->GetGUIDFromIndex(0)));
	printf("Stage 0, instance 0 challenges instance 1 (should pass)\n");
	int stage=0;
	int stage4FailureCount=0;
	int passCount=0;
	bool quit=false;

	while (!quit)
	{
		MafiaNet::Packet *packet;
		for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
		{
			packet=rakPeer[peerIndex]->Receive();
			if (packet)
			{
				switch (packet->data[0])
				{
					case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS:
					case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS:
					{
						MafiaNet::BitStream bs(packet->data, packet->length, false);
						bs.IgnoreBytes(sizeof(MafiaNet::MessageID));
						MafiaNet::RakString password;
						bs.Read(password);
						if (packet->data[0]==ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS)
							printf("ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS with %s from %s\n", password.C_String(), packet->systemAddress.ToString(true));
						else
							printf("ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS with %s from %s\n", password.C_String(), packet->systemAddress.ToString(true));
						if (++passCount==2)
						{
							if (stage<=2)
							{
								if (stage==0)
								{
									printf("Stage 1, instance 1 challenges instance 0 (should pass)\n");
									twoWayAuthenticationPlugin[1]->Challenge("PWD0", rakPeer[1]->GetGUIDFromIndex(0));
									passCount=0;
									// stage==1
								}
								else if (stage==1)
								{
									printf("Stage 2, instance 1 re-challenges instance 0 (should pass)\n");
									twoWayAuthenticationPlugin[1]->Challenge("PWD0", rakPeer[1]->GetGUIDFromIndex(0));
									passCount=0;
									// stage==2
								}
								else
								{
									printf("Stage 3, instance 0 challenges with bad password (call should fail)\n");
									if (twoWayAuthenticationPlugin[0]->Challenge("PWD3", rakPeer[0]->GetGUIDFromIndex(0))==false)
									{
										printf("Passed stage 3\n");
										stage++;
										printf("Stage 4, instance 0 challenges with unknown password (should fail twice)\n");
										twoWayAuthenticationPlugin[0]->Challenge("PWD1", rakPeer[0]->GetGUIDFromIndex(0));
									}
									else
									{
										printf("Failed stage 3, Challenge() did not return false\n");
									}
								}
								stage++;
							}
						}
						
					}
					break;
					case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE:
					{
						printf("ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILED from %s\n", packet->systemAddress.ToString(true));
						if (stage!=4)
						{
							printf("Failed stage %i\n", stage);
						}
						else
						{
							if (++stage4FailureCount==2)
								quit=true;
						}
					}
					break;
					case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT:
					{
						MafiaNet::BitStream bs(packet->data, packet->length, false);
						bs.IgnoreBytes(sizeof(MafiaNet::MessageID));
						MafiaNet::RakString password;
						bs.Read(password);
						printf("ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT with %s from %s\n", password.C_String(), packet->systemAddress.ToString(true));
						printf("Failed stage %i\n", stage);
					}
					break;
					case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE:
					{
						MafiaNet::BitStream bs(packet->data, packet->length, false);
						bs.IgnoreBytes(sizeof(MafiaNet::MessageID));
						MafiaNet::RakString password;
						bs.Read(password);
						printf("ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILED with %s from %s\n", password.C_String(), packet->systemAddress.ToString(true));
						if (stage!=4)
						{
							printf("Failed stage %i\n", stage);
						}
						else
						{
							if (++stage4FailureCount==2)
								quit=true;
						}
					}
					break;
				}
				rakPeer[peerIndex]->DeallocatePacket(packet);
			}
		}

		RakSleep(30);
	}


	for (int i=0; i < NUM_PEERS; i++)
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer[i]);

	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
		MafiaNet::OP_DELETE(twoWayAuthenticationPlugin[peerIndex], _FILE_AND_LINE_);

	return 1;
}
