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

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_PacketLogger==1

#if defined(UNICODE)
#include "mafianet/wstring.h"
#endif

#include "mafianet/packet_output_window_logger.h"
#include "mafianet/string.h"
#if defined(_WIN32)
#include "mafianet/windows_includes.h"
#endif

using namespace MafiaNet;

PacketOutputWindowLogger::PacketOutputWindowLogger()
{
}
PacketOutputWindowLogger::~PacketOutputWindowLogger()
{
}
void PacketOutputWindowLogger::WriteLog(const char *str)
{
#if defined(_WIN32)

	#if defined(UNICODE)
	MafiaNet::RakWString str2 = str;
		str2+="\n";
		OutputDebugString(str2.C_String());
	#else
	MafiaNet::RakString str2 = str;
		str2+="\n";
		OutputDebugString(str2.C_String());
	#endif
#endif
}

#endif // _RAKNET_SUPPORT_*
