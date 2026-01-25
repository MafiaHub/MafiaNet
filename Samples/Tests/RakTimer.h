/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#pragma once


#include "RakNetTime.h"
#include "GetTime.h"

using namespace MafiaNet;
class RakTimer
{
public:
	RakTimer(void);
	RakTimer(int lengthInMilliseconds);
	~RakTimer(void);
    void SetTimerLength(int lengthInMilliseconds);
    void Start();
    void Pause();
    void Resume();
	bool IsExpired();
private:
	int timerLength;//Modified by SetTimerLength
	int pauseOffset;
	TimeMS startTime;
};
