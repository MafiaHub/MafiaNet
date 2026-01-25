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

/// \file
/// \brief Tests multiple readers and writers on the same instance of RakPeer.


#include "mafianet/peerinterface.h"

#include "mafianet/GetTime.h"
#include "mafianet/statistics.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/Kbhit.h"
#include <stdio.h> // Printf
#include "mafianet/WindowsIncludes.h" // Sleep
//#include <process.h>
#include "mafianet/thread.h"
#include "mafianet/sleep.h"
using namespace MafiaNet;

RakPeerInterface *peer1, *peer2;
bool endThreads;

RAK_THREAD_DECLARATION(ProducerThread)
{
	unsigned char i = *((unsigned char *) arguments);
	unsigned char out[2];
	out[0]=(unsigned char)ID_USER_PACKET_ENUM;
	out[1]=i;

	while (endThreads==false)
	{
//		printf("Thread %u writing...\n", i);
		// #high - (char*)-cast hack to simply send unsigned char types to the peer - consider changing Send() to accept unsigned char (i.e. ID_USER_PACKET_ENUM exceeds 127)
		if (i&1)
			peer1->Send((char*)out, 2, HIGH_PRIORITY, RELIABLE_ORDERED, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
		else
			peer2->Send((char*)out, 2, HIGH_PRIORITY, RELIABLE_ORDERED, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

//		printf("Thread %u done writing\n", i);
		RakSleep(30);
	}
	
	return 0;
}

RAK_THREAD_DECLARATION(ConsumerThread)
{
	unsigned char i = *((unsigned char *) arguments);
	MafiaNet::Packet *p;
	while (endThreads==false)
	{
//		printf("Thread %u reading...\n", i);
		if (i&1)
			p=peer1->Receive();
		else
			p=peer2->Receive();
	//	printf("Thread %u done reading...\n", i);

		if (p)
		{
			if (p->data[0]==ID_USER_PACKET_ENUM)
				printf("Got data from thread %u\n", p->data[1]);
			if (i&1)
				peer1->DeallocatePacket(p);
			else
				peer2->DeallocatePacket(p);
		}

        RakSleep(30);		
	}

	return 0;
}

int main()
{
	peer1= MafiaNet::RakPeerInterface::GetInstance();
	peer2= MafiaNet::RakPeerInterface::GetInstance();
	peer1->SetMaximumIncomingConnections(1);
	peer2->SetMaximumIncomingConnections(1);
	MafiaNet::SocketDescriptor socketDescriptor(1234,0);
	peer1->Startup(1,&socketDescriptor, 1);
	socketDescriptor.port=1235;
	peer2->Startup(1,&socketDescriptor, 1);
	RakSleep(500);
	peer1->Connect("127.0.0.1", 1235, 0, 0);
	peer2->Connect("127.0.0.1", 1234, 0, 0);

	printf("Tests multiple threads sharing the same instance of RakPeer\n");
	printf("Difficulty: Beginner\n\n");



	endThreads=false;
	unsigned char count[20];
	printf("Starting threads\n");
	for (unsigned char i=0; i < 10; i++)
	{
		count[i]=i;
		MafiaNet::RakThread::Create(&ProducerThread, count+i);
	}
	for (unsigned char i=10; i < 20; i++)
	{
		count[i]=i;
		MafiaNet::RakThread::Create(&ConsumerThread, count+i);
	}

	printf("Running test\n");
	MafiaNet::TimeMS endTime = 60 * 1000 + MafiaNet::GetTimeMS();
	while (MafiaNet::GetTimeMS() < endTime)
	{

		RakSleep(0);
	}
	endThreads=true;
	printf("Test done!\n");

	MafiaNet::RakPeerInterface::DestroyInstance(peer1);
	MafiaNet::RakPeerInterface::DestroyInstance(peer2);

	return 0;
}
