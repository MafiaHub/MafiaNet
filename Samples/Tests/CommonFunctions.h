/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#pragma once


#include "mafianet/string.h"

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"
#include "DebugTools.h"
	#include "RakTimer.h"
#include "SampleSecurity.h"

using namespace MafiaNet;
class CommonFunctions
{
public:
	CommonFunctions(void);
	~CommonFunctions(void);

	static bool WaitAndConnect(RakPeerInterface *peer,char* ip,unsigned short int port,int millisecondsToWait);
	static bool WaitForMessageWithID(RakPeerInterface *reciever,int id,int millisecondsToWait);
	static Packet * WaitAndReturnMessageWithID(RakPeerInterface *reciever,int id,int millisecondsToWait);
	static void DisconnectAndWait(RakPeerInterface *peer,char* ip,unsigned short int port);
    static bool ConnectionStateMatchesOptions(RakPeerInterface *peer, SystemAddress currentSystem, bool isConnected, bool isConnecting=false, bool isPending=false, bool isDisconnecting=false, bool isNotConnected=false, bool isLoopBack=false, bool isSilentlyDisconnecting=false);
};
