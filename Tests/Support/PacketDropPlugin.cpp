/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "PacketDropPlugin.h"

PacketDropPlugin::PacketDropPlugin(void)
{
	timer.SetTimerLength(500);
}

PacketDropPlugin::~PacketDropPlugin(void)
{
}

PluginReceiveResult PacketDropPlugin::OnReceive(Packet *packet)
{
	if (timer.IsExpired())
	{
		return RR_CONTINUE_PROCESSING;
	}
	else
	{
		return RR_STOP_PROCESSING;
	}

}

void PacketDropPlugin::StartTest()
{
	timer.Start();

}
