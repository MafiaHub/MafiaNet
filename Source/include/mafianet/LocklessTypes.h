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

#ifndef __LOCKLESS_TYPES_H
#define __LOCKLESS_TYPES_H

#include "Export.h"
#include "NativeTypes.h"
#include <atomic>

namespace MafiaNet
{

// Lock-free atomic counter. Backed by std::atomic, which is portable and, unlike
// the previous platform-specific implementations, gives a race-free GetValue()
// (the old non-Windows path read 'value' without the lock that Increment/Decrement
// took, which ThreadSanitizer correctly flagged as a data race).
class RAK_DLL_EXPORT LocklessUint32_t
{
public:
	LocklessUint32_t();
	explicit LocklessUint32_t(uint32_t initial);
	// Returns variable value after changing it
	uint32_t Increment(void);
	// Returns variable value after changing it
	uint32_t Decrement(void);
	uint32_t GetValue(void) const {return value.load();}

protected:
	std::atomic<uint32_t> value;
};

}

#endif
