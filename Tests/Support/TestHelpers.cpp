/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "TestHelpers.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

TestHelpers::TestHelpers(void)
{
}

TestHelpers::~TestHelpers(void)
{
}

void TestHelpers::StandardServerPrep(RakPeerInterface *&server)
{

	server=RakPeerInterface::GetInstance();
	SocketDescriptor serverSd(60000, 0);
	server->Startup(1, &serverSd, 1);
	server->SetMaximumIncomingConnections(1);

}

void TestHelpers::StandardClientPrep(RakPeerInterface *&client)
{

	client=RakPeerInterface::GetInstance();

	SocketDescriptor clientSd;
	client->Startup(1, &clientSd, 1);

}

void TestHelpers::StandardServerPrep(RakPeerInterface *&server,DataStructures::List <RakPeerInterface *> &destroyList)
{

	StandardServerPrep(server);
	destroyList.Push(server,_FILE_AND_LINE_);

}

void TestHelpers::StandardClientPrep(RakPeerInterface *&client,DataStructures::List <RakPeerInterface *> &destroyList)
{

	StandardClientPrep(client);
	destroyList.Push(client,_FILE_AND_LINE_);

}

//returns false if not connected
bool TestHelpers::WaitAndConnectTwoPeersLocally(RakPeerInterface *connector,RakPeerInterface *connectee,int millisecondsToWait)
{

	SystemAddress connecteeAdd=connectee->GetInternalID();
	return CommonFunctions::WaitAndConnect(connector,"127.0.0.1",connecteeAdd.GetPort(),millisecondsToWait);

}

//returns false if connect fails
bool TestHelpers::ConnectTwoPeersLocally(RakPeerInterface *connector,RakPeerInterface *connectee)
{
	SystemAddress connecteeAdd=connectee->GetInternalID();
	return connector->Connect("127.0.0.1",connecteeAdd.GetPort(),0,0);
}

bool TestHelpers::BroadCastTestPacket(RakPeerInterface *sender,MafiaNet::Reliability rel,MafiaNet::Priority pr,int typeNum)//returns send return value
{

	char str2[]="AAAAAAAAAA";
	str2[0]=typeNum;
	return sender->Send(str2,(int) strlen(str2)+1, pr, rel  ,0, UNASSIGNED_SYSTEM_ADDRESS, true)>0;
}

bool TestHelpers::SendTestPacketDirected(RakPeerInterface *sender,char * ip,int port,MafiaNet::Reliability rel,MafiaNet::Priority pr,int typeNum)//returns send return value
{

	SystemAddress recAddress;

	recAddress.SetBinaryAddress(ip);
	recAddress.SetPortHostOrder(port);

	char str2[]="AAAAAAAAAA";
	str2[0]=typeNum;
	return sender->Send(str2,(int) strlen(str2)+1, pr, rel  ,0, recAddress, false)>0;
}

bool TestHelpers::WaitForTestPacket(RakPeerInterface *reciever,int millisecondsToWait)
{

	RakTimer timer(millisecondsToWait);

	Packet *packet;
	while(!timer.IsExpired())
	{
		for (packet=reciever->Receive(); packet;reciever->DeallocatePacket(packet), packet=reciever->Receive())
		{

			if (packet->data[0]==ID_USER_PACKET_ENUM+1)
			{
				reciever->DeallocatePacket(packet);
				return true;
			}

		}

	}

	return false;

}

void RecieveForXTime(RakPeerInterface *reciever,int millisecondsToWait)
{

	RakTimer timer(millisecondsToWait);

	Packet *packet;
	while(!timer.IsExpired())
	{
		for (packet=reciever->Receive(); packet;reciever->DeallocatePacket(packet), packet=reciever->Receive())
		{

		}

	}

}

unsigned short TestHelpers::FindFreeTcpPort(void)
{
#ifdef _WIN32
	// TCPInterface::Start calls WSAStartup, but this helper runs before it, so ask
	// for Winsock here too. WSAStartup/WSACleanup are reference counted.
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return 0;
#endif

	unsigned short port = 0;

#ifdef _WIN32
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s != INVALID_SOCKET)
#else
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s >= 0)
#endif
	{
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = 0; // let the OS choose
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (bind(s, (sockaddr *) &addr, sizeof(addr)) == 0)
		{
			sockaddr_in bound;
			memset(&bound, 0, sizeof(bound));
#ifdef _WIN32
			int boundLen = (int) sizeof(bound);
#else
			socklen_t boundLen = (socklen_t) sizeof(bound);
#endif
			if (getsockname(s, (sockaddr *) &bound, &boundLen) == 0)
				port = ntohs(bound.sin_port);
		}

#ifdef _WIN32
		closesocket(s);
#else
		close(s);
#endif
	}

#ifdef _WIN32
	WSACleanup();
#endif

	return port;
}

unsigned short TestHelpers::StartTcpOnAFreePort(TCPInterface &tcp, unsigned short maxIncomingConnections,
	int attempts)
{
	for (int i = 0; i < attempts; i++)
	{
		const unsigned short port = FindFreeTcpPort();
		if (port == 0)
			continue;
		if (tcp.Start(port, maxIncomingConnections))
			return port;
		// Start() tears itself down on a failed bind, so it is safe to call again.
	}
	return 0;
}
