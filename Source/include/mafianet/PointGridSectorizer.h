/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief Uniform grid spatial index over point entries with incremental
/// add/remove/move, intended as the server-side interest-management index.
///
/// Unlike GridSectorizer — whose compiled-in per-cell storage is append-only,
/// forcing consumers to Clear() and rebuild the whole grid to evict anything —
/// PointGridSectorizer keeps a per-entry record (cell + slot) so entries can be
/// removed or moved individually in O(1), independent of the cell count and the
/// total entry count. Entries are points, not boxes: each entry occupies
/// exactly one cell, so GetEntries() never returns duplicates and callers do
/// not need to dedup (GridSectorizer could return one entry once per cell its
/// box spanned).

#ifndef __POINT_GRID_SECTORIZER_H
#define __POINT_GRID_SECTORIZER_H

#include <stdint.h>

#include "Export.h"
#include "memoryoverride.h"
#include "DS_List.h"
#include "DS_Hash.h"

namespace MafiaNet
{

/// \internal Bucket count of the entry-record hash; ~3-entry chains at the
/// 50k-entry scale target. Allocated lazily on the first AddEntry (8 bytes per
/// bucket).
static const unsigned int POINT_GRID_SECTORIZER_HASH_SIZE = 16384;

/// \internal Mixes the pointer bits (splitmix64 finalizer) so allocation
/// alignment does not cluster hash buckets.
inline unsigned long PointGridSectorizerPtrHash(void * const &entry)
{
	uint64_t h = (uint64_t) (uintptr_t) entry;
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return (unsigned long) h;
}

/// \brief Spatial grid over point entries with O(1) incremental updates.
/// \details One entry per pointer: AddEntry() and MoveEntry() are the same
/// upsert operation, so position writes can be forwarded to the grid blindly
/// without tracking membership. All positions (entries and query rectangles)
/// are clamped to the world bounds given to Init() — out-of-bounds positions
/// land in the edge cells, they never assert or drop. Queries return the
/// contents of every cell overlapping the rectangle, i.e. a cell-granularity
/// superset of the exact matches; callers post-filter, as with GridSectorizer.
/// Not thread-safe; intended for single-threaded use from the update loop.
class RAK_DLL_EXPORT PointGridSectorizer
{
public:
	PointGridSectorizer();
	~PointGridSectorizer();

	/// (Re-)initializes the grid, discarding any current entries.
	/// \param[in] _cellWidth, _cellHeight Size of each cell in world units
	/// \param[in] minX, minY, maxX, maxY World bounds; positions outside clamp to the edge cells
	void Init(const float _cellWidth, const float _cellHeight, const float minX, const float minY, const float maxX, const float maxY);

	/// Adds a point entry at (x,y), clamped to the world bounds. O(1).
	/// If \a entry is already in the grid this relocates it (same as MoveEntry).
	void AddEntry(void *entry, const float x, const float y);

	/// Removes an entry. O(1) via swap-remove within its cell.
	/// \return True if the entry was in the grid, false if absent (no-op).
	bool RemoveEntry(void *entry);

	/// Moves an entry to (x,y), clamped to the world bounds. O(1); if the new
	/// position is in the entry's current cell this is a cheap early-out (the
	/// hot case for small per-tick movement). Inserts the entry if absent.
	void MoveEntry(void *entry, const float x, const float y);

	/// Clears \a intersectionList and fills it with every entry in the cells
	/// overlapping the rectangle, each exactly once. O(cells overlapped + entries returned).
	void GetEntries(DataStructures::List<void*> &intersectionList, const float minX, const float minY, const float maxX, const float maxY) const;

	/// Returns whether the entry is currently in the grid. O(1).
	bool HasEntry(void *entry);

	/// Number of entries in the grid. O(1).
	unsigned int Size(void) const;

	/// Removes all entries; the grid stays initialized and reusable. O(cell count).
	void Clear(void);

protected:
	/// \internal Where an entry lives: cell index and slot within that cell's list.
	struct EntryRecord
	{
		int cellIndex;
		unsigned int slotIndex;
	};

	int WorldToCellX(const float input) const;
	int WorldToCellY(const float input) const;
	int WorldToCellXOffsetAndClamped(const float input) const;
	int WorldToCellYOffsetAndClamped(const float input) const;
	int WorldToCellIndexClamped(const float x, const float y) const;

	void InsertIntoCell(void *entry, const int cellIndex);
	void RemoveFromCell(const EntryRecord &record);

	float cellOriginX, cellOriginY;
	float cellWidth, cellHeight;
	float invCellWidth, invCellHeight;
	float gridWidth, gridHeight;
	int gridCellWidthCount, gridCellHeightCount;

	/// Per-cell unordered entry lists; removal swaps the last element into the
	/// freed slot, so order within a cell is not meaningful.
	DataStructures::List<void*> *grid;
	DataStructures::Hash<void*, EntryRecord, POINT_GRID_SECTORIZER_HASH_SIZE, PointGridSectorizerPtrHash> entryRecords;
};

} // namespace MafiaNet

#endif // __POINT_GRID_SECTORIZER_H
