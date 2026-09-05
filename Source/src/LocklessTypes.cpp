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

#include "mafianet/LocklessTypes.h"

using namespace MafiaNet;

// std::atomic replaces the previous per-platform mix of InterlockedIncrement,
// mutex-guarded arithmetic, and __sync_fetch_and_add. Besides being standard,
// this fixes two defects of the old code: GetValue() read the counter without
// any synchronization (a data race with concurrent modifications), and the
// __sync_fetch_and_add branch returned the value from *before* the change
// while every other platform returned the value after it.

LocklessUint32_t::LocklessUint32_t()
{
	value=0;
}
LocklessUint32_t::LocklessUint32_t(uint32_t initial)
{
	value=initial;
}
uint32_t LocklessUint32_t::Increment(void)
{
	return value.fetch_add(1)+1;
}
uint32_t LocklessUint32_t::Decrement(void)
{
	return value.fetch_sub(1)-1;
}
