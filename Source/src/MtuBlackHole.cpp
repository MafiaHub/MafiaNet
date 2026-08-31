/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/MtuBlackHole.h"

#include "mafianet/MTUSize.h"

namespace MafiaNet {

const int MTU_LADDER[MTU_LADDER_SIZE] = {MAXIMUM_MTU_SIZE, 1280, 1024, 576};

int NextLowerMtu(int currentMtuBytes)
{
	for (int i = 0; i < MTU_LADDER_SIZE; i++)
	{
		if (MTU_LADDER[i] < currentMtuBytes)
			return MTU_LADDER[i];
	}
	return 0;
}

bool ShouldStepDownMtu(uint32_t timesSent, int requiredDatagramBytes, int currentMtuBytes)
{
	if (timesSent < MTU_BLACKHOLE_RESEND_THRESHOLD)
		return false;
	const int nextLower = NextLowerMtu(currentMtuBytes);
	if (nextLower == 0)
		return false;
	// Only a packet that would actually shrink is evidence of a black hole:
	// one that already fits the next rung would go out unchanged after a
	// step-down, so its failures indicate loss, not size.
	return requiredDatagramBytes > nextLower;
}

} // namespace MafiaNet
