/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "Lobby2Plugin.h"

#include <algorithm>

using namespace MafiaNet;

Lobby2Plugin::Lobby2Plugin()
{
	orderingChannel = 0;
	packetPriority = MafiaNet::Priority::High;
}
Lobby2Plugin::~Lobby2Plugin()
{
}
void Lobby2Plugin::SetOrderingChannel(char oc)
{
	orderingChannel = oc;
}
void Lobby2Plugin::SetSendPriority(MafiaNet::Priority pp)
{
	packetPriority = pp;
}
void Lobby2Plugin::SetMessageFactory(Lobby2MessageFactory *f)
{
	msgFactory = f;
}
Lobby2MessageFactory *Lobby2Plugin::GetMessageFactory(void) const
{
	return msgFactory;
}
void Lobby2Plugin::SetCallbackInterface(Lobby2Callbacks *cb)
{
	ClearCallbackInterfaces();
	callbacks.push_back(cb);
}
void Lobby2Plugin::AddCallbackInterface(Lobby2Callbacks *cb)
{
	RemoveCallbackInterface(cb);
	callbacks.push_back(cb);
}
void Lobby2Plugin::RemoveCallbackInterface(Lobby2Callbacks *cb)
{
	auto it = std::find(callbacks.begin(), callbacks.end(), cb);
	if (it != callbacks.end())
		callbacks.erase(it);
}
void Lobby2Plugin::ClearCallbackInterfaces()
{
	callbacks.clear();
}
