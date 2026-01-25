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

#include "mafianet/peerinterface.h"
#include "mafianet/FileOperations.h"
#include "mafianet/memoryoverride.h"
#include "mafianet/ConsoleServer.h"
#include "mafianet/LogCommandParser.h"
#include "mafianet/commandparser.h"
#include "mafianet/PacketLogger.h"
#include "mafianet/DS_List.h"
#include "mafianet/SocketLayer.h"
#include "mafianet/sleep.h"
#include "mafianet/TCPInterface.h"

using namespace MafiaNet;

void* MyMalloc (size_t size)
{
	return malloc(size);
}

void* MyRealloc (void *p, size_t size)
{
	return realloc(p,size);
}

void MyFree (void *p)
{
	free(p);
}

// This project is used to test the DLL system to make sure necessary classes are exported
int main()
{
	// Just test allocation and deallocation across the DLL.  If it crashes it failed, otherwise it worked.
	ConsoleServer* a=ConsoleServer::GetInstance( );
	LogCommandParser* c=LogCommandParser::GetInstance( );
	PacketLogger* d=PacketLogger::GetInstance( );
	RakNetCommandParser* e=RakNetCommandParser::GetInstance( );
	RakPeerInterface * f=RakPeerInterface::GetInstance( );
	TCPInterface *g=TCPInterface::GetInstance();
	SystemAddress sa = UNASSIGNED_SYSTEM_ADDRESS;

	SocketDescriptor sd(5555,0);
	if(f->Startup(32,&sd,1) != RAKNET_STARTED) {		
			printf("NetworkNode::startup(): failed to start server\n");
			return 0;
		}
	f->SetMaximumIncomingConnections(32);
	
	DataStructures::List<RakNetSocket2* > sockets;
	f->GetSockets(sockets);
	f->ReleaseSockets(sockets);

	// See memoryoverride.h
	SetMalloc(MyMalloc);
	SetRealloc(MyRealloc);
	SetFree(MyFree);

	char *cArray = MafiaNet::OP_NEW_ARRAY<char>(10,_FILE_AND_LINE_);
	MafiaNet::OP_DELETE_ARRAY(cArray,_FILE_AND_LINE_);

	DataStructures::List<int> intList;
	intList.Push(5, _FILE_AND_LINE_ );
	
	f->GetMTUSize(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
	SystemAddress p1;
	SystemAddress p2;
	p1=p2;

	RakSleep(300);


	ConsoleServer::DestroyInstance( a );
	LogCommandParser::DestroyInstance( c );
	PacketLogger::DestroyInstance( d );
	RakNetCommandParser::DestroyInstance( e );
	MafiaNet::RakPeerInterface::DestroyInstance( f );
	TCPInterface::DestroyInstance( g );

	return 0;
}

