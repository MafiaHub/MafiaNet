/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#pragma once


#include "mafianet/string.h"

#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "DebugTools.h"
#include "CommonFunctions.h"
#include "RakTimer.h"

using namespace MafiaNet;
class TestHelpers
{
public:
	TestHelpers(void);
	~TestHelpers(void);

	// Starts peer on 127.0.0.1 with a port the OS assigns, so tests can run in
	// parallel. Returns that port (0 if Startup failed). Never bind a fixed
	// port in a test.
	static unsigned short StartOnLoopback(RakPeerInterface *peer, unsigned int maxConnections, unsigned short maxIncoming = 0);
	// The port a started peer is bound to.
	static unsigned short BoundPort(RakPeerInterface *peer);

	// Server on an OS-assigned port: read it with BoundPort(server).
	static void StandardServerPrep(RakPeerInterface *&server);
	static void StandardClientPrep(RakPeerInterface *&client);
	static void StandardServerPrep(RakPeerInterface *&server, DataStructures::List<RakPeerInterface *> &destroyList);
	static void StandardClientPrep(RakPeerInterface *&client, DataStructures::List<RakPeerInterface *> &destroyList);

	static bool WaitAndConnectTwoPeersLocally(RakPeerInterface *connector, RakPeerInterface *connectee, int millisecondsToWait);
	static bool ConnectTwoPeersLocally(RakPeerInterface *connector, RakPeerInterface *connectee);
	///static bool BroadCastTestPacket(RakPeerInterface *sender);
	static bool BroadCastTestPacket(RakPeerInterface *sender, MafiaNet::Reliability rel = MafiaNet::Reliability::ReliableOrdered, MafiaNet::Priority pr = MafiaNet::Priority::High, int typeNum = ID_USER_PACKET_ENUM + 1);
	static bool WaitForTestPacket(RakPeerInterface *reciever, int millisecondsToWait);
	static void RecieveForXTime(RakPeerInterface *reciever, int millisecondsToWait);
	static bool SendTestPacketDirected(RakPeerInterface *sender, char *ip, int port, MafiaNet::Reliability rel = MafiaNet::Reliability::ReliableOrdered, MafiaNet::Priority pr = MafiaNet::Priority::High, int typeNum = ID_USER_PACKET_ENUM + 1);
};
