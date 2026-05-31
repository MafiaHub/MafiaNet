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

#include "mafianet/string.h"

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"
#include "DebugTools.h"
#include "CommonFunctions.h"

using namespace MafiaNet;
class PeerConnectDisconnectWithCancelPendingTest : public TestInterface
{
public:
	PeerConnectDisconnectWithCancelPendingTest(void);
	~PeerConnectDisconnectWithCancelPendingTest(void);
	int RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses);//should return 0 if no error, or the error number
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
private:
	DataStructures::List <RakPeerInterface *> destroyList;
};
