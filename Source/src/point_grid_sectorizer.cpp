/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/point_grid_sectorizer.h"
#include "mafianet/vector_util.h"
#include <math.h>
#include <stdint.h>

using namespace MafiaNet;

static const unsigned int POINT_GRID_INITIAL_RECORD_CAPACITY = 64;

// Mixes the pointer bits (splitmix64 finalizer) so allocation alignment does
// not cluster table slots.
static inline uint64_t PointGridPtrHash(void *entry)
{
	uint64_t h = (uint64_t) (uintptr_t) entry;
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return h;
}

PointGridSectorizer::PointGridSectorizer()
{
	grid = 0;
	records = 0;
	recordCapacity = 0;
	recordCount = 0;
	cellOriginX = cellOriginY = 0.0f;
	invCellWidth = invCellHeight = 0.0f;
	gridCellWidthCount = gridCellHeightCount = 0;
}
PointGridSectorizer::~PointGridSectorizer()
{
	if (grid)
		delete[] grid;
	if (records)
		delete[] records;
}
bool PointGridSectorizer::Init(const float _cellWidth, const float _cellHeight, const float minX, const float minY, const float maxX, const float maxY)
{
	// Tear down any previous state first; on failure the grid stays inert.
	if (grid)
	{
		delete[] grid;
		grid = 0;
	}
	if (records)
	{
		delete[] records;
		records = 0;
	}
	recordCapacity = 0;
	recordCount = 0;
	gridCellWidthCount = gridCellHeightCount = 0;

	// Invalid parameters are reported through the return value rather than an
	// assert so the failure path stays exercisable in debug builds; the
	// negated comparisons also reject NaN parameters.
	if (!(_cellWidth > 0.0f) || !(_cellHeight > 0.0f) || !(minX < maxX) || !(minY < maxY))
		return false;

	const float gridWidth = maxX - minX;
	const float gridHeight = maxY - minY;
	const double cellsWide = ceil((double) gridWidth / _cellWidth);
	const double cellsHigh = ceil((double) gridHeight / _cellHeight);
	// Computed in double so a huge world / tiny cell combination is caught
	// here instead of wrapping the int cell count (and the allocation size).
	if (!(cellsWide >= 1.0) || !(cellsHigh >= 1.0) || cellsWide * cellsHigh > 2147483647.0)
		return false;

	cellOriginX = minX;
	cellOriginY = minY;
	gridCellWidthCount = (int) cellsWide;
	gridCellHeightCount = (int) cellsHigh;
	// Make the cells slightly smaller, so we allocate an extra unneeded cell if on the edge.  This way we don't go outside the array on rounding errors.
	invCellWidth = (float) gridCellWidthCount / gridWidth;
	invCellHeight = (float) gridCellHeightCount / gridHeight;

	// Records before grid: if the second allocation throws, grid==0 still
	// marks the instance inert and the destructor frees what was allocated.
	records = new EntryRecord[POINT_GRID_INITIAL_RECORD_CAPACITY];
	recordCapacity = POINT_GRID_INITIAL_RECORD_CAPACITY;
	for (unsigned int i = 0; i < recordCapacity; ++i)
		records[i].entry = 0;
	grid = new std::vector<void *>[gridCellWidthCount * gridCellHeightCount];
	return true;
}
bool PointGridSectorizer::AddEntry(void *entry, const float x, const float y)
{
	// Same upsert as MoveEntry: one record per pointer, relocate if already present.
	return MoveEntry(entry, x, y);
}
bool PointGridSectorizer::RemoveEntry(void *entry)
{
	if (grid == 0 || entry == 0)
		return false;

	EntryRecord *record = FindRecord(entry);
	if (record == 0)
		return false;
	RemoveFromCell(*record);
	EraseRecord(record);
	return true;
}
bool PointGridSectorizer::MoveEntry(void *entry, const float x, const float y)
{
	// A null entry would corrupt the record table (null marks empty slots).
	if (grid == 0 || entry == 0)
		return false;

	const int cellIndex = WorldToCellIndexClamped(x, y);
	EntryRecord *record = FindRecord(entry);
	if (record == 0)
	{
		InsertRecord(entry, cellIndex, PushIntoCell(entry, cellIndex));
		return true;
	}
	if (record->cellIndex == cellIndex)
		return false;

	RemoveFromCell(*record);
	record->cellIndex = cellIndex;
	record->slotIndex = PushIntoCell(entry, cellIndex);
	return true;
}
void PointGridSectorizer::GetEntries(std::vector<void *> &intersectionList, const float minX, const float minY, const float maxX, const float maxY) const
{
	// clear() keeps the buffer, so a reused query list keeps its
	// high-water-mark capacity instead of regrowing every call.
	intersectionList.clear();
	if (grid == 0)
		return;

	const int xStart = WorldToCellXClamped(minX);
	const int yStart = WorldToCellYClamped(minY);
	const int xEnd = WorldToCellXClamped(maxX);
	const int yEnd = WorldToCellYClamped(maxY);

	for (int yCur = yStart; yCur <= yEnd; ++yCur)
	{
		// Row-major: consecutive cells of a row are adjacent in memory, so
		// sweeping mostly-empty regions stays cache-friendly.
		const std::vector<void *> *row = grid + yCur * gridCellWidthCount;
		for (int xCur = xStart; xCur <= xEnd; ++xCur)
		{
			const std::vector<void *> &cell = row[xCur];
			intersectionList.insert(intersectionList.end(), cell.begin(), cell.end());
		}
	}
}
bool PointGridSectorizer::HasEntry(void *entry) const
{
	return entry != 0 && FindRecord(entry) != 0;
}
unsigned int PointGridSectorizer::Size(void) const
{
	return recordCount;
}
void PointGridSectorizer::Clear(void)
{
	if (grid == 0)
		return;
	const int count = gridCellWidthCount * gridCellHeightCount;
	for (int cur = 0; cur < count; cur++)
		grid[cur].clear();
	for (unsigned int i = 0; i < recordCapacity; ++i)
		records[i].entry = 0;
	recordCount = 0;
}
PointGridSectorizer::EntryRecord *PointGridSectorizer::FindRecord(void *entry) const
{
	if (records == 0)
		return 0;
	const unsigned int mask = recordCapacity - 1;
	unsigned int i = (unsigned int) PointGridPtrHash(entry) & mask;
	while (records[i].entry != 0)
	{
		if (records[i].entry == entry)
			return &records[i];
		i = (i + 1) & mask;
	}
	return 0;
}
void PointGridSectorizer::InsertRecord(void *entry, const int cellIndex, const unsigned int slotIndex)
{
	// Grow at 75% load so linear probes stay short.
	if (recordCount + 1 > recordCapacity - (recordCapacity / 4))
		GrowRecordTable();
	const unsigned int mask = recordCapacity - 1;
	unsigned int i = (unsigned int) PointGridPtrHash(entry) & mask;
	while (records[i].entry != 0)
		i = (i + 1) & mask;
	records[i].entry = entry;
	records[i].cellIndex = cellIndex;
	records[i].slotIndex = slotIndex;
	recordCount++;
}
void PointGridSectorizer::EraseRecord(EntryRecord *record)
{
	const unsigned int mask = recordCapacity - 1;
	unsigned int hole = (unsigned int) (record - records);
	records[hole].entry = 0;
	// Backward-shift deletion: pull each follower whose home slot is cyclically
	// outside (hole, follower] back into the hole, so probe chains stay intact
	// without tombstones.
	unsigned int follower = hole;
	for (;;)
	{
		follower = (follower + 1) & mask;
		if (records[follower].entry == 0)
			break;
		const unsigned int home = (unsigned int) PointGridPtrHash(records[follower].entry) & mask;
		const bool canFillHole = (follower > hole) ? (home <= hole || home > follower) : (home <= hole && home > follower);
		if (canFillHole)
		{
			records[hole] = records[follower];
			records[follower].entry = 0;
			hole = follower;
		}
	}
	recordCount--;
}
void PointGridSectorizer::GrowRecordTable(void)
{
	// Allocate and rehash into a temporary first: records/recordCapacity are
	// only updated once the new table exists, so a throwing allocation cannot
	// leave a doubled capacity (and probe mask) over the old half-size array.
	const unsigned int newCapacity = recordCapacity * 2;
	EntryRecord *newRecords = new EntryRecord[newCapacity];
	for (unsigned int i = 0; i < newCapacity; ++i)
		newRecords[i].entry = 0;
	const unsigned int mask = newCapacity - 1;
	for (unsigned int i = 0; i < recordCapacity; ++i)
	{
		if (records[i].entry == 0)
			continue;
		unsigned int j = (unsigned int) PointGridPtrHash(records[i].entry) & mask;
		while (newRecords[j].entry != 0)
			j = (j + 1) & mask;
		newRecords[j] = records[i];
	}
	delete[] records;
	records = newRecords;
	recordCapacity = newCapacity;
}
unsigned int PointGridSectorizer::PushIntoCell(void *entry, const int cellIndex)
{
	std::vector<void *> &cell = grid[cellIndex];
	cell.push_back(entry);
	return (unsigned int) (cell.size() - 1);
}
void PointGridSectorizer::RemoveFromCell(const EntryRecord &record)
{
	std::vector<void *> &cell = grid[record.cellIndex];
	void *last = cell.back();
	if (last != record.entry)
	{
		// Swap-remove: the last entry will fill the freed slot; fix its stored slot.
		FindRecord(last)->slotIndex = record.slotIndex;
	}
	SwapRemove(cell, record.slotIndex);
}
int PointGridSectorizer::WorldToCellXClamped(const float input) const
{
	// Clamp in float space BEFORE the int cast: casting NaN or a value beyond
	// int range to int is undefined behavior (and saturates differently per
	// platform). The negated comparison sends NaN to cell 0.
	const float cell = (input - cellOriginX) * invCellWidth;
	if (!(cell > 0.0f))
		return 0;
	if (cell >= (float) gridCellWidthCount)
		return gridCellWidthCount - 1;
	const int asInt = (int) cell;
	return asInt < gridCellWidthCount ? asInt : gridCellWidthCount - 1;
}
int PointGridSectorizer::WorldToCellYClamped(const float input) const
{
	const float cell = (input - cellOriginY) * invCellHeight;
	if (!(cell > 0.0f))
		return 0;
	if (cell >= (float) gridCellHeightCount)
		return gridCellHeightCount - 1;
	const int asInt = (int) cell;
	return asInt < gridCellHeightCount ? asInt : gridCellHeightCount - 1;
}
int PointGridSectorizer::WorldToCellIndexClamped(const float x, const float y) const
{
	return WorldToCellYClamped(y) * gridCellWidthCount + WorldToCellXClamped(x);
}
