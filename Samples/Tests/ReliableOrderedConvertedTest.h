/*
 *  Copyright (c) 2014, Oculus VR, Inc.
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

#pragma once


#include "TestInterface.h"


#include "mafianet/peerinterface.h"
#include "mafianet/GetTime.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include <cstdio>
#include <memory.h>
#include <cstring>
#include <stdlib.h>
#include "mafianet/Rand.h"
#include "mafianet/statistics.h"
#include "mafianet/sleep.h"
#include "mafianet/memoryoverride.h"

#include "DebugTools.h"

using namespace MafiaNet;
class ReliableOrderedConvertedTest : public TestInterface
{
public:
	ReliableOrderedConvertedTest(void);
	~ReliableOrderedConvertedTest(void);
	int RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses);//should return 0 if no error, or the error number
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
protected:
	void *LoggedMalloc(size_t size, const char *file, unsigned int line);
	void LoggedFree(void *p, const char *file, unsigned int line);
	void* LoggedRealloc(void *p, size_t size, const char *file, unsigned int line);
private:
	DataStructures::List <RakPeerInterface *> destroyList;

};
