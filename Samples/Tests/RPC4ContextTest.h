/*
 *  Copyright (c) 2024, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#pragma once


#include "TestInterface.h"

#include "mafianet/string.h"
#include "mafianet/DS_List.h"
#include "mafianet/peerinterface.h"

using namespace MafiaNet;

/// Verifies that RPC4 slot/function handlers receive the per-registration
/// user-context pointer passed at registration time, removing the need for
/// file-static global pointers to route an RPC back to an object instance.
class RPC4ContextTest : public TestInterface
{
public:
	RPC4ContextTest(void);
	~RPC4ContextTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();

private:
	DataStructures::List<RakString> errorList;
	DataStructures::List<RakPeerInterface *> destroyList;
};
