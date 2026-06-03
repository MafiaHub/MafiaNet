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

/// Verifies the optional disconnect-reason payload carried on
/// ID_DISCONNECTION_NOTIFICATION via the CloseConnection() reasonData overload.
///
/// Two live loopback peers are connected, then the server kicks the client:
/// - With a reason BitStream (an enum byte + a RakString): the client's
///   ID_DISCONNECTION_NOTIFICATION must carry the exact payload after the 1-byte
///   ID (packet->data+1, length packet->length-1), and it must round-trip.
/// - With no reason (nullptr, the default): the notification stays a bare
///   1-byte message (packet->length == 1), proving wire-backward-compatibility
///   and that consumers can rely on a zero-length body.
class DisconnectReasonTest : public TestInterface
{
public:
	DisconnectReasonTest(void);
	~DisconnectReasonTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();

private:
	DataStructures::List<RakString> errorList;
	DataStructures::List<RakPeerInterface *> destroyList;
};
