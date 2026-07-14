/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/PointGridSectorizer.h"

#include <limits>
#include <type_traits>

using namespace MafiaNet;

// The grid owns raw memory (cell lists and the entry-record table); a
// memberwise copy would double-free it. The type must not be copyable.
static_assert(!std::is_copy_constructible<PointGridSectorizer>::value,
	"PointGridSectorizer must not be copy-constructible (owns raw memory)");
static_assert(!std::is_copy_assignable<PointGridSectorizer>::value,
	"PointGridSectorizer must not be copy-assignable (owns raw memory)");

namespace
{
	// Opaque entity pointers for the grid: addresses into a static array.
	int dummies[1024];
	void *E(int i) { return &dummies[i]; }

	// Number of times ptr occurs in the list (point entries must never duplicate).
	unsigned int CountOf(const DataStructures::List<void *> &list, void *ptr)
	{
		unsigned int count = 0;
		for (unsigned int i = 0; i < list.Size(); ++i)
			if (list[i] == ptr)
				++count;
		return count;
	}
}

// 100x100 world of 10x10 cells -> a 10x10 grid.
class PointGridSectorizerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(grid.Init(10.0f, 10.0f, 0.0f, 0.0f, 100.0f, 100.0f));
	}

	PointGridSectorizer grid;
	DataStructures::List<void *> hits;
};

TEST_F(PointGridSectorizerTest, AddAndQueryReturnsOnlyEntriesInRectangle)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);
	grid.AddEntry(E(1), 55.0f, 55.0f);
	grid.AddEntry(E(2), 95.0f, 95.0f);

	grid.GetEntries(hits, 0.0f, 0.0f, 20.0f, 20.0f);
	EXPECT_EQ(CountOf(hits, E(0)), 1u);
	EXPECT_TRUE(grid.HasEntry(E(0)));
	EXPECT_EQ(grid.Size(), 3u);
	EXPECT_EQ(CountOf(hits, E(1)), 0u) << "query returned an entry from a distant cell";
	EXPECT_EQ(CountOf(hits, E(2)), 0u) << "query returned an entry from a distant cell";

	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(hits.Size(), 3u);
	EXPECT_EQ(CountOf(hits, E(0)), 1u);
	EXPECT_EQ(CountOf(hits, E(1)), 1u);
	EXPECT_EQ(CountOf(hits, E(2)), 1u);
}

TEST_F(PointGridSectorizerTest, RemovePresentEntry)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);
	grid.AddEntry(E(1), 55.0f, 55.0f);

	EXPECT_TRUE(grid.RemoveEntry(E(1)));
	EXPECT_FALSE(grid.HasEntry(E(1)));
	EXPECT_EQ(grid.Size(), 1u);

	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(hits.Size(), 1u);
	EXPECT_EQ(CountOf(hits, E(1)), 0u) << "removed entry still returned by a query";
}

TEST_F(PointGridSectorizerTest, RemoveAbsentEntryIsNoOp)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);

	EXPECT_FALSE(grid.RemoveEntry(E(1)));
	EXPECT_FALSE(grid.RemoveEntry(E(200)));
	EXPECT_EQ(grid.Size(), 1u);
}

TEST_F(PointGridSectorizerTest, MoveWithinSameCellKeepsEntry)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);

	grid.MoveEntry(E(0), 7.0f, 7.0f);
	grid.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	EXPECT_EQ(CountOf(hits, E(0)), 1u);
	EXPECT_EQ(grid.Size(), 1u);
}

TEST_F(PointGridSectorizerTest, MoveAcrossCellsRelocatesEntryExactlyOnce)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);
	grid.AddEntry(E(1), 55.0f, 55.0f);

	grid.MoveEntry(E(0), 85.0f, 85.0f);
	grid.GetEntries(hits, 0.0f, 0.0f, 20.0f, 20.0f);
	EXPECT_EQ(CountOf(hits, E(0)), 0u) << "entry still found in its old cell";
	grid.GetEntries(hits, 80.0f, 80.0f, 99.0f, 99.0f);
	EXPECT_EQ(CountOf(hits, E(0)), 1u) << "entry not found in its new cell";
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(hits.Size(), 2u);
	EXPECT_EQ(CountOf(hits, E(0)), 1u) << "cross-cell move duplicated or dropped the entry";
}

TEST_F(PointGridSectorizerTest, MoveEntryUpsertsAbsentEntry)
{
	grid.MoveEntry(E(3), 15.0f, 15.0f);
	grid.GetEntries(hits, 10.0f, 10.0f, 19.0f, 19.0f);
	EXPECT_TRUE(grid.HasEntry(E(3)));
	EXPECT_EQ(CountOf(hits, E(3)), 1u);
	EXPECT_EQ(grid.Size(), 1u);
}

TEST_F(PointGridSectorizerTest, ReAddingExistingEntryRelocatesIt)
{
	grid.AddEntry(E(2), 95.0f, 95.0f);

	grid.AddEntry(E(2), 15.0f, 15.0f);
	grid.GetEntries(hits, 90.0f, 90.0f, 99.0f, 99.0f);
	EXPECT_EQ(CountOf(hits, E(2)), 0u) << "re-adding an entry left it in its old cell";
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(grid.Size(), 1u);
	EXPECT_EQ(CountOf(hits, E(2)), 1u) << "re-adding an entry duplicated it";
}

TEST_F(PointGridSectorizerTest, OutOfBoundsPositionsClampToEdgeCells)
{
	grid.AddEntry(E(4), -50.0f, 50.0f);   // clamps to x cell 0
	grid.AddEntry(E(5), 150.0f, 150.0f);  // clamps to cell (9,9)

	grid.GetEntries(hits, 0.0f, 50.0f, 9.0f, 59.0f);
	EXPECT_EQ(CountOf(hits, E(4)), 1u) << "entry added at x=-50 not clamped into the x=0 edge cell";
	grid.GetEntries(hits, 90.0f, 90.0f, 99.0f, 99.0f);
	EXPECT_EQ(CountOf(hits, E(5)), 1u) << "entry added at (150,150) not clamped into the (9,9) edge cell";
}

TEST_F(PointGridSectorizerTest, QueryRectanglesBeyondBoundsClamp)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);
	grid.AddEntry(E(5), 150.0f, 150.0f); // clamps to cell (9,9)

	grid.GetEntries(hits, -1000.0f, -1000.0f, 1000.0f, 1000.0f);
	EXPECT_EQ(hits.Size(), grid.Size()) << "oversized query did not return all entries";

	grid.GetEntries(hits, 200.0f, 200.0f, 300.0f, 300.0f); // clamps to the (9,9) edge cell
	EXPECT_EQ(CountOf(hits, E(5)), 1u) << "query entirely beyond bounds did not clamp to the edge cell";
}

// Swap-remove slot fixup: many entries in one cell, interleaved removals.
TEST_F(PointGridSectorizerTest, SwapRemoveBookkeepingSurvivesInterleavedRemovals)
{
	PointGridSectorizer dense;
	ASSERT_TRUE(dense.Init(10.0f, 10.0f, 0.0f, 0.0f, 100.0f, 100.0f));
	const int denseCount = 64;
	int i;
	for (i = 0; i < denseCount; ++i)
		dense.AddEntry(E(i), 5.0f, 5.0f); // all in cell (0,0)
	for (i = 0; i < denseCount; i += 2)
		ASSERT_TRUE(dense.RemoveEntry(E(i))) << "interleaved RemoveEntry(" << i << ") failed";

	dense.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	EXPECT_EQ(dense.Size(), (unsigned int) denseCount / 2);
	EXPECT_EQ(hits.Size(), (unsigned int) denseCount / 2);
	for (i = 0; i < denseCount; ++i)
	{
		EXPECT_EQ(CountOf(hits, E(i)), (i % 2 == 0 ? 0u : 1u))
			<< "entry " << i << " membership wrong after interleaved removals";
	}

	// Moves out of a dense cell also use the swap-remove path.
	for (i = 1; i <= 15; i += 2)
		dense.MoveEntry(E(i), 55.0f, 55.0f);
	dense.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	EXPECT_EQ(hits.Size(), (unsigned int) denseCount / 2 - 8);
	dense.GetEntries(hits, 50.0f, 50.0f, 59.0f, 59.0f);
	EXPECT_EQ(hits.Size(), 8u);

	for (i = 0; i < denseCount; ++i)
		dense.RemoveEntry(E(i));
	dense.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(dense.Size(), 0u);
	EXPECT_EQ(hits.Size(), 0u);
}

TEST_F(PointGridSectorizerTest, ClearEmptiesGridAndLeavesItUsable)
{
	grid.AddEntry(E(0), 5.0f, 5.0f);
	grid.AddEntry(E(1), 55.0f, 55.0f);

	grid.Clear();
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(grid.Size(), 0u);
	EXPECT_EQ(hits.Size(), 0u);
	EXPECT_FALSE(grid.HasEntry(E(0)));

	grid.AddEntry(E(0), 25.0f, 25.0f);
	grid.GetEntries(hits, 20.0f, 20.0f, 29.0f, 29.0f);
	EXPECT_EQ(grid.Size(), 1u);
	EXPECT_EQ(CountOf(hits, E(0)), 1u) << "grid unusable after Clear";
}

// Extreme and non-finite positions stay defined and clamp to edge cells.
// (The int cast must happen after clamping in float space: casting 1e30f or
// NaN to int is UB and saturates differently per platform.)
TEST_F(PointGridSectorizerTest, ExtremeAndNonFinitePositionsClamp)
{
	grid.AddEntry(E(10), 1.0e30f, 1.0e30f);   // far past max -> cell (9,9)
	grid.AddEntry(E(11), -1.0e30f, -1.0e30f); // far past min -> cell (0,0)
	const float nan = std::numeric_limits<float>::quiet_NaN();
	grid.AddEntry(E(12), nan, nan);           // non-finite -> defined: cell (0,0)

	grid.GetEntries(hits, 90.0f, 90.0f, 99.0f, 99.0f);
	EXPECT_EQ(CountOf(hits, E(10)), 1u) << "entry at +1e30 not clamped to the max edge cell";
	grid.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	EXPECT_EQ(CountOf(hits, E(11)), 1u) << "entry at -1e30 not clamped to the min edge cell";
	EXPECT_EQ(CountOf(hits, E(12)), 1u) << "entry at NaN not clamped to the min edge cell";

	grid.GetEntries(hits, -1.0e30f, nan, 1.0e30f, 1.0e30f); // query rect is clamped the same way
	EXPECT_EQ(grid.Size(), 3u);
	EXPECT_EQ(CountOf(hits, E(10)), 1u);
	EXPECT_EQ(CountOf(hits, E(11)), 1u);
}

TEST(PointGridSectorizerLifecycle, UseBeforeInitIsDefinedNoOp)
{
	PointGridSectorizer uninitialized;
	DataStructures::List<void *> hits;

	uninitialized.AddEntry(E(0), 5.0f, 5.0f);
	uninitialized.MoveEntry(E(0), 15.0f, 15.0f);
	uninitialized.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	uninitialized.Clear();
	EXPECT_FALSE(uninitialized.RemoveEntry(E(0)));
	EXPECT_FALSE(uninitialized.HasEntry(E(0)));
	EXPECT_EQ(uninitialized.Size(), 0u);
	EXPECT_EQ(hits.Size(), 0u);
}

TEST(PointGridSectorizerLifecycle, DegenerateInitFailsCleanlyAndRecovers)
{
	PointGridSectorizer degenerate;
	DataStructures::List<void *> hits;

	EXPECT_FALSE(degenerate.Init(10.0f, 10.0f, 0.0f, 0.0f, 0.0f, 50.0f));   // zero-width world
	EXPECT_FALSE(degenerate.Init(-5.0f, 10.0f, 0.0f, 0.0f, 100.0f, 100.0f)); // negative cell size

	degenerate.AddEntry(E(0), 5.0f, 5.0f);
	degenerate.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_EQ(degenerate.Size(), 0u) << "grid not inert after failed Init";
	EXPECT_EQ(hits.Size(), 0u);

	ASSERT_TRUE(degenerate.Init(10.0f, 10.0f, 0.0f, 0.0f, 100.0f, 100.0f))
		<< "valid re-Init after failed Init did not recover";
	degenerate.AddEntry(E(0), 5.0f, 5.0f);
	degenerate.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	EXPECT_EQ(CountOf(hits, E(0)), 1u) << "grid unusable after recovering from failed Init";
}

TEST(PointGridSectorizerLifecycle, CellCountOverflowingIntIsRejected)
{
	PointGridSectorizer huge;
	EXPECT_FALSE(huge.Init(0.0001f, 0.0001f, 0.0f, 0.0f, 1.0e6f, 1.0e6f)); // 1e10 x 1e10 cells
	huge.AddEntry(E(0), 5.0f, 5.0f);
	EXPECT_EQ(huge.Size(), 0u) << "grid not inert after overflowing Init";
}

TEST_F(PointGridSectorizerTest, AddAndMoveReportCellTransitions)
{
	EXPECT_TRUE(grid.MoveEntry(E(0), 5.0f, 5.0f));    // insert
	EXPECT_FALSE(grid.MoveEntry(E(0), 7.0f, 7.0f));   // same cell
	EXPECT_TRUE(grid.MoveEntry(E(0), 55.0f, 55.0f));  // cross cell
	EXPECT_FALSE(grid.AddEntry(E(0), 56.0f, 56.0f));  // same cell via AddEntry
	EXPECT_TRUE(grid.AddEntry(E(1), 5.0f, 5.0f));     // insert via AddEntry

	PointGridSectorizer uninitialized;
	EXPECT_FALSE(uninitialized.MoveEntry(E(0), 5.0f, 5.0f)); // pre-Init -> no transition

	EXPECT_FALSE(grid.AddEntry(0, 5.0f, 5.0f)); // null entry rejected
	EXPECT_EQ(grid.Size(), 2u);

	// Read-only access must work through a const reference.
	const PointGridSectorizer &constGrid = grid;
	constGrid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	EXPECT_TRUE(constGrid.HasEntry(E(0)));
	EXPECT_EQ(constGrid.Size(), 2u);
	EXPECT_EQ(hits.Size(), 2u);
}

// Randomized differential stress: interleaved add/move/remove against a
// reference model, growing the record bookkeeping well past its initial size.
TEST_F(PointGridSectorizerTest, RandomizedStressMatchesReferenceModel)
{
	const int stressEntities = 600;
	bool present[600] = {false};
	unsigned int liveCount = 0;
	unsigned int rng = 0x12345678u; // deterministic LCG
	for (int iter = 0; iter < 30000; ++iter)
	{
		rng = rng*1664525u + 1013904223u;
		const int target = (int)((rng >> 8) % stressEntities);
		const float px = (float)((rng >> 4) % 120) - 10.0f;  // includes out-of-bounds
		const float py = (float)((rng >> 12) % 120) - 10.0f;
		switch ((rng >> 28) % 3)
		{
		case 0: // upsert via AddEntry
			if (grid.AddEntry(E(target), px, py) && !present[target])
				++liveCount;
			present[target] = true;
			break;
		case 1: // upsert via MoveEntry
			grid.MoveEntry(E(target), px, py);
			if (!present[target])
				++liveCount;
			present[target] = true;
			break;
		case 2: // remove
			ASSERT_EQ(grid.RemoveEntry(E(target)), present[target])
				<< "RemoveEntry disagreed with the model at iteration " << iter;
			if (present[target])
				--liveCount;
			present[target] = false;
			break;
		}
		ASSERT_EQ(grid.Size(), liveCount) << "at iteration " << iter;
		if ((iter % 977) == 0)
		{
			grid.GetEntries(hits, -100.0f, -100.0f, 200.0f, 200.0f);
			ASSERT_EQ(hits.Size(), liveCount) << "at iteration " << iter;
			for (int i = 0; i < stressEntities; ++i)
			{
				ASSERT_EQ(CountOf(hits, E(i)), (present[i] ? 1u : 0u))
					<< "entry " << i << " membership wrong at iteration " << iter;
				ASSERT_EQ(grid.HasEntry(E(i)), present[i])
					<< "entry " << i << " membership wrong at iteration " << iter;
			}
		}
	}
}
