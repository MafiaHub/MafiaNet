/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Generating a GUID must not sleep. Every RakPeer generates one when it is constructed,
// so a sleeping generator is paid by every peer: on Windows a Sleep(1) lasts a whole timer
// tick (~15.6 ms), and eight of them made each peer cost ~125 ms, which was most of the
// unit suite's run time there. This reads the wall clock, so it lives in the integration
// suite.

#include <gtest/gtest.h>

#include "mafianet/get_time.h"
#include "mafianet/peer_interface.h"

using namespace MafiaNet;

TEST(PeerGuidTiming, GeneratingGuidsDoesNotSleep)
{
	// A sleep costs at least a millisecond even at the finest Windows timer resolution
	// (another process may have raised it from the default 15.6 ms), so 256 calls that
	// each slept once could not finish in 100 ms. Without sleeping they take microseconds.
	const int kGuids = 256;
	const TimeMS start = GetTimeMS();
	for (int i = 0; i < kGuids; ++i)
		(void) RakPeerInterface::Get64BitUniqueRandomNumber();
	const TimeMS elapsed = GetTimeMS() - start;

	EXPECT_LT(elapsed, 100u) << kGuids << " GUIDs took " << elapsed << " ms";
}
