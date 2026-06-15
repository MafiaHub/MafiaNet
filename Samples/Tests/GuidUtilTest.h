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
#include "mafianet/peerinterface.h"

using namespace MafiaNet;

/// Verifies the modern value-type accessors from "mafianet/guid_util.h" (issue
/// #15):
/// - to_string(guid) returns the same text as the legacy thread-safe
///   ToString(char*, size_t) member, handles the UNASSIGNED sentinel, and is
///   safe to call concurrently from many threads (no shared static buffer).
/// - connected_address(peer, guid) maps the UNASSIGNED_SYSTEM_ADDRESS "none"
///   sentinel to std::nullopt and otherwise forwards the legacy lookup.
class GuidUtilTest : public TestInterface
{
public:
	GuidUtilTest(void);
	~GuidUtilTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();

private:
	DataStructures::List<RakPeerInterface *> destroyList;
};
