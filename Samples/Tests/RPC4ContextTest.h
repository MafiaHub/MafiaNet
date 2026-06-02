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
#include "mafianet/RPC4Plugin.h"

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
	// Plugins are members so they outlive the peers: the peers are shut down in
	// DestroyPeers() (after RunTest returns), and RakPeer::Shutdown() calls back
	// into attached plugins. A plugin destroyed before its peer would be a
	// use-after-free during shutdown (~PluginInterface2 does not auto-detach).
	RPC4 clientRpc;
	RPC4 serverRpc;
};
