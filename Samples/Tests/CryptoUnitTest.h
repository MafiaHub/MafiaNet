#pragma once

#include "TestInterface.h"

#include "mafianet/string.h"

using namespace MafiaNet;

class CryptoUnitTest : public TestInterface
{
public:
    CryptoUnitTest(void);
    ~CryptoUnitTest(void);
    int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
    RakString GetTestName();
    RakString ErrorCodeToString(int errorCode);
    void DestroyPeers() {}
};
