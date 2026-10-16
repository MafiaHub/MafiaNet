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

#include "mafianet/lockless_types.h"

using namespace MafiaNet;

LocklessUint32_t::LocklessUint32_t() : value(0)
{
}
LocklessUint32_t::LocklessUint32_t(uint32_t initial) : value(initial)
{
}
// GetValue() used to read the counter without the lock or barrier the writers
// used, and the Linux path returned the value *before* the change, contrary to
// the header. std::atomic fixes both on every platform.
uint32_t LocklessUint32_t::Increment(void)
{
	return value.fetch_add(1) + 1;
}
uint32_t LocklessUint32_t::Decrement(void)
{
	return value.fetch_sub(1) - 1;
}
