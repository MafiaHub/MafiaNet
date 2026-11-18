/*
 *  Copyright (c) 2014, Oculus VR, Inc.
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

#include "mafianet/itoa.h"

#include <algorithm>
#include <charconv>
#include <limits>

char *Itoa(int value, char *result, int base)
{
	if (base < 2 || base > 16)
	{
		*result = '\0';
		return result;
	}

	// The caller may have space only for the actual result. Give to_chars its own
	// range, large enough for every binary digit and a possible minus sign.
	char digits[std::numeric_limits<unsigned int>::digits + 1];
	const auto converted = std::to_chars(digits, digits + sizeof(digits), value, base);
	char *first = digits;
	if (value < 0 && base != 10)
		++first; // Itoa uses the magnitude for nondecimal bases.
	*std::copy(first, converted.ptr, result) = '\0';
	return result;
}
