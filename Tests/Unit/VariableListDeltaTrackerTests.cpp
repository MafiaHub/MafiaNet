/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/variable_list_delta_tracker.h"
#include "mafianet/string.h"

using namespace MafiaNet;

namespace
{
	unsigned char DirtyBit(unsigned int index)
	{
		return static_cast<unsigned char>(0x80 >> (index & 7));
	}
}

TEST(VariableListDeltaTracker, UnchangedValueIsNotWrittenAgain)
{
	VariableListDeltaTracker tracker;
	tracker.StartWrite();
	EXPECT_TRUE(tracker.WriteVar(RakString("a")));
	tracker.StartWrite();
	EXPECT_FALSE(tracker.WriteVar(RakString("a")));
}

TEST(VariableListDeltaTracker, ResizedValueKeepsTheNextVariableDirty)
{
	VariableListDeltaTracker tracker;
	tracker.StartWrite();
	tracker.WriteVar(RakString("a"));
	tracker.WriteVar(5);

	unsigned char dirty[1] = { DirtyBit(1) };
	tracker.FlagDirtyFromBitArray(dirty);

	tracker.StartWrite();
	EXPECT_TRUE(tracker.WriteVar(RakString("abc")));
	EXPECT_TRUE(tracker.WriteVar(5));
}

TEST(VariableListDeltaTracker, ResizedValueClearsItsOwnDirtyFlag)
{
	VariableListDeltaTracker tracker;
	tracker.StartWrite();
	tracker.WriteVar(RakString("a"));
	tracker.WriteVar(5);

	unsigned char dirty[1] = { DirtyBit(0) };
	tracker.FlagDirtyFromBitArray(dirty);

	tracker.StartWrite();
	EXPECT_TRUE(tracker.WriteVar(RakString("abc")));
	tracker.WriteVar(5);

	tracker.StartWrite();
	EXPECT_FALSE(tracker.WriteVar(RakString("abc")));
}

TEST(VariableListDeltaTracker, ResizingTheLastVariableStaysInBounds)
{
	VariableListDeltaTracker tracker;
	tracker.StartWrite();
	tracker.WriteVar(RakString("a"));

	tracker.StartWrite();
	EXPECT_TRUE(tracker.WriteVar(RakString("abc")));
	EXPECT_TRUE(tracker.IsPastEndOfList());

	tracker.StartWrite();
	EXPECT_FALSE(tracker.WriteVar(RakString("abc")));
}
