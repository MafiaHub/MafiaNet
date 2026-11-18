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
#if _RAKNET_SUPPORT_PacketLogger == 1

#include "mafianet/packet_output_window_logger.h"
#include <string>
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
void PacketOutputWindowLogger::WriteLog([[maybe_unused]] const char *str) // only Windows has an output window
{
#if defined(_WIN32)
	const std::string line = std::string(str ? str : "") + '\n';
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.c_str(), -1, nullptr, 0);
	if (length > 0)
	{
		std::wstring wide(static_cast<size_t>(length), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.c_str(), -1, wide.data(), length))
		{
			OutputDebugStringW(wide.c_str());
			return;
		}
	}
	// Legacy callers may log text in the Windows ANSI code page.
	OutputDebugStringA(line.c_str());
#endif
}

#endif // _RAKNET_SUPPORT_*
