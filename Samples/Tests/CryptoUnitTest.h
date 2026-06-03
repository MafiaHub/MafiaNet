/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#pragma once

#include "TestInterface.h"

#include "mafianet/string.h"

using namespace MafiaNet;

class CryptoUnitTest : public TestInterface
{
public:
    CryptoUnitTest(void);
    ~CryptoUnitTest() override;
    int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
    RakString GetTestName();
    RakString ErrorCodeToString(int errorCode);
    void DestroyPeers() {}
};
