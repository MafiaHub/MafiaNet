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
#include "BitStream.h"

using namespace MafiaNet;

// Regression test for the std::string serialization trap: the catch-all
// BitStream::Write/Read templates used to raw-copy a std::string's internal
// representation (pointer/size/capacity) instead of its characters, producing
// aliased heap pointers and eventual double-frees. std::string now has a
// dedicated specialization that uses the same length-prefixed wire format as
// RakString. This test verifies the round-trip and wire-compatibility.
class BitStreamStringTest : public TestInterface
{
public:
    BitStreamStringTest(void);
    ~BitStreamStringTest(void);
    int RunTest(DataStructures::List<RakString> params,bool isVerbose,bool noPauses);//should return 0 if no error, or the error number
    RakString GetTestName();
    RakString ErrorCodeToString(int errorCode);
    void DestroyPeers();
};
