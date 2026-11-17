/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_PacketLogger == 1

#include "mafianet/threadsafe_packet_logger.h"

using namespace MafiaNet;

ThreadsafePacketLogger::ThreadsafePacketLogger()
{
}
ThreadsafePacketLogger::~ThreadsafePacketLogger()
{
}
void ThreadsafePacketLogger::Update(void)
{
	while (auto msg = logMessages.TryPop())
		WriteLog(msg->c_str());
}
void ThreadsafePacketLogger::AddToLog(const char *str)
{
	logMessages.Push(std::string(str));
}

#endif // _RAKNET_SUPPORT_*
