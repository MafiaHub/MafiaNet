/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "PointGridSectorizerTest.h"

#include "mafianet/PointGridSectorizer.h"

#include <stdio.h>

// Opaque entity pointers for the grid: addresses into a static array.
static int dummies[256];
static void *E(int i) { return &dummies[i]; }

// Number of times ptr occurs in the list (point entries must never duplicate).
static unsigned int CountOf(const DataStructures::List<void *> &list, void *ptr)
{
	unsigned int count = 0;
	for (unsigned int i = 0; i < list.Size(); ++i)
		if (list[i] == ptr)
			++count;
	return count;
}

int PointGridSectorizerTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void) params; (void) noPauses;

	// 100x100 world of 10x10 cells -> a 10x10 grid.
	PointGridSectorizer grid;
	grid.Init(10.0f, 10.0f, 0.0f, 0.0f, 100.0f, 100.0f);

	DataStructures::List<void *> hits;

	// --- Add/query ---
	grid.AddEntry(E(0), 5.0f, 5.0f);
	grid.AddEntry(E(1), 55.0f, 55.0f);
	grid.AddEntry(E(2), 95.0f, 95.0f);

	grid.GetEntries(hits, 0.0f, 0.0f, 20.0f, 20.0f);
	if (CountOf(hits, E(0)) != 1 || grid.HasEntry(E(0)) == false || grid.Size() != 3)
	{
		if (isVerbose) printf("query around (5,5) did not return the entry added there\n");
		return 1;
	}
	if (CountOf(hits, E(1)) != 0 || CountOf(hits, E(2)) != 0)
	{
		if (isVerbose) printf("query around (5,5) returned entries from distant cells\n");
		return 2;
	}

	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	if (hits.Size() != 3 || CountOf(hits, E(0)) != 1 || CountOf(hits, E(1)) != 1 || CountOf(hits, E(2)) != 1)
	{
		if (isVerbose) printf("full-world query did not return each entry exactly once\n");
		return 3;
	}

	// --- Remove: present ---
	if (grid.RemoveEntry(E(1)) == false || grid.HasEntry(E(1)) || grid.Size() != 2)
	{
		if (isVerbose) printf("RemoveEntry did not remove a present entry\n");
		return 4;
	}
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	if (hits.Size() != 2 || CountOf(hits, E(1)) != 0)
	{
		if (isVerbose) printf("removed entry still returned by a query\n");
		return 4;
	}

	// --- Remove: absent (no-op) ---
	if (grid.RemoveEntry(E(1)) == true || grid.RemoveEntry(E(200)) == true || grid.Size() != 2)
	{
		if (isVerbose) printf("RemoveEntry of an absent entry was not a no-op\n");
		return 5;
	}

	// --- Move within the same cell ---
	grid.MoveEntry(E(0), 7.0f, 7.0f);
	grid.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	if (CountOf(hits, E(0)) != 1 || grid.Size() != 2)
	{
		if (isVerbose) printf("entry lost by a move within its cell\n");
		return 6;
	}

	// --- Move across cells ---
	grid.MoveEntry(E(0), 85.0f, 85.0f);
	grid.GetEntries(hits, 0.0f, 0.0f, 20.0f, 20.0f);
	if (CountOf(hits, E(0)) != 0)
	{
		if (isVerbose) printf("entry still found in its old cell after a cross-cell move\n");
		return 7;
	}
	grid.GetEntries(hits, 80.0f, 80.0f, 99.0f, 99.0f);
	if (CountOf(hits, E(0)) != 1)
	{
		if (isVerbose) printf("entry not found in its new cell after a cross-cell move\n");
		return 8;
	}
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	if (hits.Size() != 2 || CountOf(hits, E(0)) != 1)
	{
		if (isVerbose) printf("cross-cell move duplicated or dropped an entry\n");
		return 8;
	}

	// --- MoveEntry upserts an absent entry ---
	grid.MoveEntry(E(3), 15.0f, 15.0f);
	grid.GetEntries(hits, 10.0f, 10.0f, 19.0f, 19.0f);
	if (grid.HasEntry(E(3)) == false || CountOf(hits, E(3)) != 1 || grid.Size() != 3)
	{
		if (isVerbose) printf("MoveEntry of an absent entry did not insert it\n");
		return 9;
	}

	// --- AddEntry of an existing entry relocates it (one record per pointer) ---
	grid.AddEntry(E(2), 15.0f, 15.0f); // was at (95,95)
	grid.GetEntries(hits, 90.0f, 90.0f, 99.0f, 99.0f);
	if (CountOf(hits, E(2)) != 0)
	{
		if (isVerbose) printf("re-adding an entry left it in its old cell\n");
		return 10;
	}
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	if (grid.Size() != 3 || CountOf(hits, E(2)) != 1)
	{
		if (isVerbose) printf("re-adding an entry duplicated it\n");
		return 10;
	}

	// --- Out-of-bounds positions clamp to edge cells ---
	grid.AddEntry(E(4), -50.0f, 50.0f);   // clamps to x cell 0
	grid.AddEntry(E(5), 150.0f, 150.0f);  // clamps to cell (9,9)
	grid.GetEntries(hits, 0.0f, 50.0f, 9.0f, 59.0f);
	if (CountOf(hits, E(4)) != 1)
	{
		if (isVerbose) printf("entry added at x=-50 not clamped into the x=0 edge cell\n");
		return 11;
	}
	grid.GetEntries(hits, 90.0f, 90.0f, 99.0f, 99.0f);
	if (CountOf(hits, E(5)) != 1)
	{
		if (isVerbose) printf("entry added at (150,150) not clamped into the (9,9) edge cell\n");
		return 11;
	}

	// --- Query rectangles at and beyond world bounds clamp too ---
	grid.GetEntries(hits, -1000.0f, -1000.0f, 1000.0f, 1000.0f);
	if (hits.Size() != grid.Size())
	{
		if (isVerbose) printf("oversized query did not return all %u entries (got %u)\n",
			grid.Size(), hits.Size());
		return 12;
	}
	grid.GetEntries(hits, 200.0f, 200.0f, 300.0f, 300.0f); // clamps to the (9,9) edge cell
	if (CountOf(hits, E(5)) != 1)
	{
		if (isVerbose) printf("query entirely beyond bounds did not clamp to the edge cell\n");
		return 12;
	}

	// --- Swap-remove slot fixup: many entries in one cell, interleaved removals ---
	PointGridSectorizer dense;
	dense.Init(10.0f, 10.0f, 0.0f, 0.0f, 100.0f, 100.0f);
	const int denseCount = 64;
	int i;
	for (i = 0; i < denseCount; ++i)
		dense.AddEntry(E(i), 5.0f, 5.0f); // all in cell (0,0)
	for (i = 0; i < denseCount; i += 2)
	{
		if (dense.RemoveEntry(E(i)) == false)
		{
			if (isVerbose) printf("interleaved RemoveEntry(%d) failed\n", i);
			return 13;
		}
	}
	dense.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	if (dense.Size() != denseCount / 2 || hits.Size() != (unsigned int) denseCount / 2)
	{
		if (isVerbose) printf("wrong count after interleaved removals: size %u, hits %u\n",
			dense.Size(), hits.Size());
		return 13;
	}
	for (i = 0; i < denseCount; ++i)
	{
		if (CountOf(hits, E(i)) != (i % 2 == 0 ? 0u : 1u))
		{
			if (isVerbose) printf("entry %d %s the cell after interleaved removals\n",
				i, i % 2 == 0 ? "still in" : "missing from");
			return 13;
		}
	}
	// Moves out of a dense cell also use the swap-remove path.
	for (i = 1; i <= 15; i += 2)
		dense.MoveEntry(E(i), 55.0f, 55.0f);
	dense.GetEntries(hits, 0.0f, 0.0f, 9.0f, 9.0f);
	if (hits.Size() != denseCount / 2 - 8)
	{
		if (isVerbose) printf("wrong source-cell count after dense moves: %u\n", hits.Size());
		return 13;
	}
	dense.GetEntries(hits, 50.0f, 50.0f, 59.0f, 59.0f);
	if (hits.Size() != 8)
	{
		if (isVerbose) printf("wrong destination-cell count after dense moves: %u\n", hits.Size());
		return 13;
	}
	for (i = 0; i < denseCount; ++i)
		dense.RemoveEntry(E(i));
	dense.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	if (dense.Size() != 0 || hits.Size() != 0)
	{
		if (isVerbose) printf("grid not empty after removing every entry\n");
		return 13;
	}

	// --- Clear, then reuse ---
	grid.Clear();
	grid.GetEntries(hits, 0.0f, 0.0f, 100.0f, 100.0f);
	if (grid.Size() != 0 || hits.Size() != 0 || grid.HasEntry(E(0)))
	{
		if (isVerbose) printf("Clear did not empty the grid\n");
		return 14;
	}
	grid.AddEntry(E(0), 25.0f, 25.0f);
	grid.GetEntries(hits, 20.0f, 20.0f, 29.0f, 29.0f);
	if (grid.Size() != 1 || CountOf(hits, E(0)) != 1)
	{
		if (isVerbose) printf("grid unusable after Clear\n");
		return 14;
	}

	printf("PointGridSectorizer add/remove/move/query behave incrementally\n");
	return 0;
}

PointGridSectorizerTest::PointGridSectorizerTest(void)
{
}

PointGridSectorizerTest::~PointGridSectorizerTest(void)
{
}

RakString PointGridSectorizerTest::GetTestName(void)
{
	return "PointGridSectorizerTest";
}

RakString PointGridSectorizerTest::ErrorCodeToString(int errorCode)
{
	switch (errorCode)
	{
	case 0:  return "No error";
	case 1:  return "Added entry not returned by a query of its cell";
	case 2:  return "Query returned entries from cells outside the rectangle";
	case 3:  return "Full-world query did not return each entry exactly once";
	case 4:  return "RemoveEntry did not remove a present entry";
	case 5:  return "RemoveEntry of an absent entry was not a no-op";
	case 6:  return "Entry lost by a move within its cell";
	case 7:  return "Entry still in its old cell after a cross-cell move";
	case 8:  return "Entry missing or duplicated after a cross-cell move";
	case 9:  return "MoveEntry of an absent entry did not insert it";
	case 10: return "AddEntry of an existing entry did not relocate it";
	case 11: return "Out-of-bounds position was not clamped to an edge cell";
	case 12: return "Query rectangle beyond world bounds was not clamped";
	case 13: return "Swap-remove bookkeeping broke under interleaved removals";
	case 14: return "Grid unusable after Clear";
	default: return "Undefined Error";
	}
}

void PointGridSectorizerTest::DestroyPeers(void)
{
}
