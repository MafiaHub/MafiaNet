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

#include "slikenet/peerinterface.h"
#include "slikenet/GetTime.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/BitStream.h"
#include <cstdio>
#include <memory.h>
#include <cstring>
#include <stdlib.h>
#include "slikenet/Rand.h"
#include "slikenet/statistics.h"
#include "slikenet/sleep.h"
#include "slikenet/memoryoverride.h"

using namespace MafiaNet;

int main(int, char **)
{
	RakPeerInterface *sender, *receiver;

	printf("This project tests sending messages of various sizes.\n");
	sender = MafiaNet::RakPeerInterface::GetInstance();
	receiver = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::SocketDescriptor sd1(1234,0),sd2(1235,0);
	receiver->Startup(32, &sd1, 1);
	receiver->SetMaximumIncomingConnections(32);
	sender->Startup(1, &sd2, 1);
	sender->Connect("127.0.0.1", 1234, 0, 0);
	RakSleep(100);

	unsigned char data[4000];
	data[0]=ID_USER_PACKET_ENUM;
	for (unsigned int i=1; i < 4000; i++)
		data[i]=i%256;
	int stride, sum;
	int sendCount, receiveCount;
	for (stride=1; stride < 2000; stride++)
	{
		sendCount=0;
		receiveCount=0;
		for (sum=0; sum < 4000; sum+=stride)
		{
			sender->Send((const char*) data,stride,HIGH_PRIORITY,RELIABLE_ORDERED,0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,true);
			sendCount++;
		}

		MafiaNet::Packet *p;
		for (p=sender->Receive(); p; sender->DeallocatePacket(p), p=sender->Receive())
			;

		MafiaNet::Time timeout= MafiaNet::GetTime()+1000;
		while (MafiaNet::GetTime()<timeout)
		{
			for (p=receiver->Receive(); p; receiver->DeallocatePacket(p), p=receiver->Receive())
			{
				if (p->data[0]==ID_USER_PACKET_ENUM)
				{
					receiveCount++;
				}
				for (unsigned int i=1; i < p->length; i++)
				{
					RakAssert(data[i]==i%256);
				}
			}
			RakSleep(30);
			if (receiveCount==sendCount)
				break;
		}

		if (sendCount==receiveCount)
			printf("Stride=%i Sends=%i Receives=%i\n", stride, sendCount, receiveCount);
		else
			printf("ERROR! Stride=%i Sends=%i Receives=%i\n", stride, sendCount, receiveCount);
	}

	if (sender)
		MafiaNet::RakPeerInterface::DestroyInstance(sender);
	if (receiver)
		MafiaNet::RakPeerInterface::DestroyInstance(receiver);

	return 1;
}
