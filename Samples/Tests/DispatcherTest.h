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
#include "mafianet/Dispatcher.h"

using namespace MafiaNet;

/// Verifies the typed message dispatcher (issue #20):
/// - Round-trip: a struct registered with on<T>() is encoded, sent over a live
///   loopback connection, and dispatched to its handler with equal fields.
/// - ID_TIMESTAMP-prefixed packets dispatch to the correct typed handler (the
///   dispatcher skips the MessageID + Time prefix just like GetPacketIdentifier).
/// - System identifiers (ID_NEW_INCOMING_CONNECTION) dispatch via on(id, ...).
/// - Sender exposes the sending guid()/address().
/// - Explicit-id registration (on<T>(id, ...)) and the auto-assigned id contract.
/// - dispatch() returns false for unregistered identifiers so the raw switch
///   path stays usable.
class DispatcherTest : public TestInterface
{
public:
	DispatcherTest(void);
	~DispatcherTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};
