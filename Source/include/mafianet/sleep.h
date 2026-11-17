/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#ifndef __RAK_SLEEP_H
#define __RAK_SLEEP_H

#include "mafianet/export.h"

/// Sleep for the requested milliseconds, or yield to the scheduler when ms is zero.
void RAK_DLL_EXPORT RakSleep(unsigned int ms);

#endif
