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

/// Verifies the typed send/broadcast overloads (issue #21):
/// - Peer::send(dispatcher, msg, to) delivers a typed message to one system with
///   the default Priority::High / Reliability::ReliableOrdered / channel 0, and
///   round-trips through the Dispatcher on the far end with equal fields.
/// - The reliability argument is overridable per call.
/// - Peer::broadcast(dispatcher, msg) reaches every connected system.
class TypedSendTest : public TestInterface
{
public:
	TypedSendTest(void);
	~TypedSendTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};
