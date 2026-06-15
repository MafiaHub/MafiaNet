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

/// Verifies the RAII handles Peer / PacketPtr (issue #16):
/// - Peer is movable and non-copyable; a moved-from handle is empty (no
///   double-free at scope exit).
/// - PacketPtr::id() returns 255 for a null/empty packet, the first byte for a
///   plain packet, and the post-timestamp byte for an ID_TIMESTAMP packet.
/// - Peer::receive() yields an empty PacketPtr when nothing is queued, and the
///   move-assignment in a receive loop frees each real packet without leaking.
class PeerHandleTest : public TestInterface
{
public:
	PeerHandleTest(void);
	~PeerHandleTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};
