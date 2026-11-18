/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#ifndef __BASE_64_ENCODER_H
#define __BASE_64_ENCODER_H

#include "mafianet/export.h"

#include <optional>
#include <string>
#include <string_view>

namespace MafiaNet
{
enum class Base64Format
{
	Standard, // Padded RFC 4648, no whitespace.
	Mime      // 76-column lines terminated by CRLF. Decoding accepts ASCII space, tab and line endings.
};

/// Encode arbitrary bytes, including embedded NULs.
RAK_DLL_EXPORT std::string EncodeBase64(std::string_view bytes, Base64Format format = Base64Format::Standard);
/// Return the decoded bytes, or std::nullopt for invalid or partially consumed input.
RAK_DLL_EXPORT std::optional<std::string> DecodeBase64(std::string_view text, Base64Format format = Base64Format::Standard);
}

extern "C"
{
	/// Legacy MIME encoder. Supply at least 2 * dataLength + 6 bytes, including space for the terminator.
	/// Returns the byte count excluding the terminator, or -1 on error.
	RAK_DLL_EXPORT int Base64Encoding(const unsigned char *inputData, int dataLength, char *outputData);
	RAK_DLL_EXPORT const char *Base64Map(void);
}

#endif
