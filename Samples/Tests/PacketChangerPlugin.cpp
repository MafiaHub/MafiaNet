/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "PacketChangerPlugin.h"

PacketChangerPlugin::PacketChangerPlugin(void)
{
}

PacketChangerPlugin::~PacketChangerPlugin(void)
{
}

void PacketChangerPlugin::OnInternalPacket(InternalPacket *internalPacket, unsigned frameNumber, SystemAddress remoteSystemAddress, TimeMS time, int isSend)
{
	// Only rewrite user messages. OnInternalPacket also fires for RakNet's internal
	// protocol traffic (e.g. the connection handshake); rewriting those would corrupt
	// the connection, which matters now that this plugin is attached before the peer
	// connects (required for thread-safe attach of a reliability-layer plugin).
	if (internalPacket->dataBitLength >= 8 && internalPacket->data[0] >= (unsigned char)ID_USER_PACKET_ENUM)
		internalPacket->data[0]=ID_USER_PACKET_ENUM+2;

	(void) frameNumber; (void) remoteSystemAddress; (void) time; (void) isSend;

}
