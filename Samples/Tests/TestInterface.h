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


#include "RakString.h"
#include "DS_List.h"

using namespace MafiaNet;
class TestInterface
{
public:
	TestInterface();
	virtual ~TestInterface();
	virtual int RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses)=0;//should return 0 if no error, or the error number
	virtual RakString GetTestName()=0;
	virtual RakString ErrorCodeToString(int errorCode)=0;
	virtual void DestroyPeers()=0;
};
