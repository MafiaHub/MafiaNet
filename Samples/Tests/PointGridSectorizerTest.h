/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#pragma once


#include "TestInterface.h"

#include "mafianet/string.h"
#include "mafianet/DS_List.h"

using namespace MafiaNet;

/// Verifies PointGridSectorizer, the incremental point-entry spatial grid:
/// add/query, O(1) remove (present and absent), move within and across cells,
/// clamping of out-of-bounds positions and query rectangles, swap-remove slot
/// fixup under interleaved removals, and Clear-then-reuse.
class PointGridSectorizerTest : public TestInterface
{
public:
	PointGridSectorizerTest(void);
	~PointGridSectorizerTest(void);
	int RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses);
	RakString GetTestName();
	RakString ErrorCodeToString(int errorCode);
	void DestroyPeers();
};
