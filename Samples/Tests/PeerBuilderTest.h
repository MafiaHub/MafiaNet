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
#include "mafianet/PeerHandle.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"

using namespace MafiaNet;

/// Verifies the Startup builder for Peer (issue #18):
/// - Peer::server().port(p).max_connections(n).incoming_password(pw).start()
///   replaces the manual Startup + SetMaximumIncomingConnections (+
///   SetIncomingPassword) sequence and a client built with
///   Peer::client().max_connections(1).connect(host, port, pw).start() actually
///   connects over loopback.
/// - The failure path is a Result<Peer> that exposes the underlying engine enum,
///   not a bool: a port clash surfaces the StartupResult (PeerStage::Startup) and
///   an unresolvable target surfaces the ConnectionAttemptResult (PeerStage::Connect).
/// - A client builder with no .connect() target still starts up (Startup only).
class PeerBuilderTest : public TestInterface
{
public:
	PeerBuilderTest(void);
	~PeerBuilderTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};
