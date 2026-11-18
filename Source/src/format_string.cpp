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

#include "mafianet/format_string.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "mafianet/linux_strings.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

char *FormatString(const char *format, ...)
{
	static int textIndex = 0;
	static char text[4][8096];
	va_list ap;
	va_start(ap, format);

	if (++textIndex == 4)
		textIndex = 0;
	vsnprintf_s(text[textIndex], 8096, _TRUNCATE, format, ap);
	va_end(ap);

	return text[textIndex];
}

char *FormatStringTS(char *output, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsnprintf_s(output, 512, _TRUNCATE, format, ap);
	va_end(ap);
	return output;
}

std::string MafiaNet::FormatText(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int length = vsnprintf(nullptr, 0, format, args);
	va_end(args);
	if (length < 0)
		return {};
	std::string result(static_cast<size_t>(length), '\0');
	va_start(args, format);
	vsnprintf(result.data(), result.size() + 1, format, args);
	va_end(args);
	return result;
}
