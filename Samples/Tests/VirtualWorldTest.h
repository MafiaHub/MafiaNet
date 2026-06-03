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

/// Verifies the virtual world (dimension) feature layered on ReplicaManager3:
/// a per-entity / per-observer virtual world id that scopes visibility without
/// moving connections or replicas between heavyweight RM3 worlds.
///
/// Success conditions:
/// - VirtualWorldsCanSee() implements equality-or-global visibility.
/// - VirtualWorldReplica3 filters QueryConstruction / QueryDestruction /
///   QuerySerialization by virtual world, delegating to the within-world
///   behavior only when the observer can see it.
/// - Connection_RM3 carries an observer virtual world (default + round-trip).
/// - ReplicaManager3 registry queries return the connections / guids in a
///   given virtual world, honoring the global sentinel.
/// - SetPlayerVirtualWorld sets both the observer and the avatar entity.
class VirtualWorldTest : public TestInterface
{
public:
	VirtualWorldTest(void);
	~VirtualWorldTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();

private:
	DataStructures::List<RakString> errorList;
	DataStructures::List<RakPeerInterface *> destroyList;
};
