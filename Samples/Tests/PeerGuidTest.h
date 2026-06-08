/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#pragma once


#include "TestInterface.h"

#include "mafianet/string.h"
#include "mafianet/DS_List.h"

using namespace MafiaNet;

/// Verifies the native strong-typed PeerGuid (issue #25): that it is distinct
/// from NetworkID at compile time, round-trips through ToPeerGuid()/ToGuid(),
/// and serializes byte-identically to the raw uint64_t it replaces.
class PeerGuidTest : public TestInterface
{
public:
	PeerGuidTest(void);
	~PeerGuidTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};
