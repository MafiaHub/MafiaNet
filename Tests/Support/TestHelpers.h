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
#include "mafianet/tcp_interface.h"
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

	static void StandardServerPrep(RakPeerInterface *&server);
	static void StandardClientPrep(RakPeerInterface *&client);
		static void StandardServerPrep(RakPeerInterface *&server,DataStructures::List <RakPeerInterface *> &destroyList);
	static void StandardClientPrep(RakPeerInterface *&client,DataStructures::List <RakPeerInterface *> &destroyList);

	static bool WaitAndConnectTwoPeersLocally(RakPeerInterface *connector,RakPeerInterface *connectee,int millisecondsToWait);
	static bool ConnectTwoPeersLocally(RakPeerInterface *connector,RakPeerInterface *connectee);
	///static bool BroadCastTestPacket(RakPeerInterface *sender);
	static bool BroadCastTestPacket(RakPeerInterface *sender,MafiaNet::Reliability rel=MafiaNet::Reliability::ReliableOrdered,MafiaNet::Priority pr=MafiaNet::Priority::High,int typeNum=ID_USER_PACKET_ENUM+1);
	static bool WaitForTestPacket(RakPeerInterface *reciever,int millisecondsToWait);
	static void RecieveForXTime(RakPeerInterface *reciever,int millisecondsToWait);
	static bool SendTestPacketDirected(RakPeerInterface *sender,char * ip,int port,MafiaNet::Reliability rel=MafiaNet::Reliability::ReliableOrdered,MafiaNet::Priority pr=MafiaNet::Priority::High,int typeNum=ID_USER_PACKET_ENUM+1);

	/// Asks the OS for a free TCP port on the loopback interface and gives it back.
	///
	/// TCPInterface::Start takes a port and exposes no accessor for the one it bound,
	/// so a test that needs a listening TCPInterface cannot use port 0 and read the
	/// result back the way the peer tests do. This binds a throwaway socket to port 0,
	/// reads the assigned port and closes it, so the caller gets a port that was free a
	/// moment ago instead of a hard-coded one. Returns 0 if no port could be obtained.
	static unsigned short FindFreeTcpPort(void);

	/// Starts \a tcp listening on a port that was free, retrying on a different
	/// port if the bind loses the race.
	///
	/// FindFreeTcpPort probes 127.0.0.1 while TCPInterface binds INADDR_ANY, and
	/// the OS hands out ephemeral numbers again soon after a test releases them,
	/// so a bind can fail because the port is in use on another interface or still
	/// in TIME_WAIT from an earlier connection -- TCPInterface sets no
	/// SO_REUSEADDR. Retrying the port selection absorbs that; it does not retry
	/// anything the caller is asserting about.
	///
	/// \return the port it is listening on, or 0 if every attempt failed.
	static unsigned short StartTcpOnAFreePort(TCPInterface &tcp, unsigned short maxIncomingConnections,
		int attempts = 20);

};
