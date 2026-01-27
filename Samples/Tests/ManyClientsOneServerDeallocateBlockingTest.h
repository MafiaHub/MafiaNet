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

#include "TestInterface.h"

#include "RakString.h"

#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include "RakPeer.h"
#include "RakSleep.h"
#include "RakNetTime.h"
#include "GetTime.h"
#include "DebugTools.h"
#include "CommonFunctions.h"

using namespace MafiaNet;
class ManyClientsOneServerDeallocateBlockingTest :
	public TestInterface
{
public:
	ManyClientsOneServerDeallocateBlockingTest(void);
	~ManyClientsOneServerDeallocateBlockingTest(void);
	int RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses);//should return 0 if no error, or the error number
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();

protected:
	void WaitForConnectionRequestsToComplete(RakPeerInterface **peerList, int peerNum, bool isVerbose);
	void WaitAndPrintResults(RakPeerInterface **peerList, int peerNum, bool isVerbose,RakPeerInterface *server);
private:
	// Reduced from 256 for stability - many clients creates too much resource pressure
	static const int clientNum= 8;
	RakPeerInterface *clientList[clientNum];//A list of clients
	RakPeerInterface *server;
};
