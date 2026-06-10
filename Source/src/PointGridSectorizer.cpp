/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/PointGridSectorizer.h"
#include <math.h>

using namespace MafiaNet;

PointGridSectorizer::PointGridSectorizer()
{
	grid=0;
	cellOriginX=cellOriginY=0.0f;
	invCellWidth=invCellHeight=0.0f;
	gridCellWidthCount=gridCellHeightCount=0;
}
PointGridSectorizer::~PointGridSectorizer()
{
	if (grid)
		MafiaNet::OP_DELETE_ARRAY(grid, _FILE_AND_LINE_);
}
bool PointGridSectorizer::Init(const float _cellWidth, const float _cellHeight, const float minX, const float minY, const float maxX, const float maxY)
{
	// Tear down any previous state first; on failure the grid stays inert.
	if (grid)
	{
		MafiaNet::OP_DELETE_ARRAY(grid, _FILE_AND_LINE_);
		grid=0;
	}
	entryRecords.Clear(_FILE_AND_LINE_);
	gridCellWidthCount=gridCellHeightCount=0;

	// Invalid parameters are reported through the return value rather than an
	// assert so the failure path stays exercisable in debug builds; the
	// negated comparisons also reject NaN parameters.
	if (!(_cellWidth > 0.0f) || !(_cellHeight > 0.0f) || !(minX < maxX) || !(minY < maxY))
		return false;

	const float gridWidth=maxX-minX;
	const float gridHeight=maxY-minY;
	const double cellsWide=ceil((double) gridWidth/_cellWidth);
	const double cellsHigh=ceil((double) gridHeight/_cellHeight);
	// Computed in double so a huge world / tiny cell combination is caught
	// here instead of wrapping the int cell count (and the allocation size).
	if (!(cellsWide >= 1.0) || !(cellsHigh >= 1.0) || cellsWide*cellsHigh > 2147483647.0)
		return false;
	cellOriginX=minX;
	cellOriginY=minY;
	gridCellWidthCount=(int) cellsWide;
	gridCellHeightCount=(int) cellsHigh;
	// Make the cells slightly smaller, so we allocate an extra unneeded cell if on the edge.  This way we don't go outside the array on rounding errors.
	invCellWidth = (float) gridCellWidthCount / gridWidth;
	invCellHeight = (float) gridCellHeightCount / gridHeight;

	grid = MafiaNet::OP_NEW_ARRAY<DataStructures::List<void*> >(gridCellWidthCount*gridCellHeightCount, _FILE_AND_LINE_ );
	return true;
}
void PointGridSectorizer::AddEntry(void *entry, const float x, const float y)
{
	// Same upsert as MoveEntry: one record per pointer, relocate if already present.
	MoveEntry(entry, x, y);
}
bool PointGridSectorizer::RemoveEntry(void *entry)
{
	if (grid==0)
		return false;

	EntryRecord *record = entryRecords.Peek(entry);
	if (record==0)
		return false;
	RemoveFromCell(*record);
	entryRecords.Remove(entry, _FILE_AND_LINE_);
	return true;
}
void PointGridSectorizer::MoveEntry(void *entry, const float x, const float y)
{
	if (grid==0)
		return;

	const int cellIndex = WorldToCellIndexClamped(x, y);
	EntryRecord *record = entryRecords.Peek(entry);
	if (record==0)
	{
		InsertIntoCell(entry, cellIndex);
		return;
	}
	if (record->cellIndex==cellIndex)
		return;

	RemoveFromCell(*record);
	DataStructures::List<void*> &cell = grid[cellIndex];
	cell.Push(entry, _FILE_AND_LINE_);
	record->cellIndex=cellIndex;
	record->slotIndex=cell.Size()-1;
}
void PointGridSectorizer::GetEntries(DataStructures::List<void*> &intersectionList, const float minX, const float minY, const float maxX, const float maxY) const
{
	intersectionList.Clear(true, _FILE_AND_LINE_);
	if (grid==0)
		return;

	const DataStructures::List<void*> *cell;
	int xStart, yStart, xEnd, yEnd, xCur, yCur;
	unsigned index;
	xStart=WorldToCellXClamped(minX);
	yStart=WorldToCellYClamped(minY);
	xEnd=WorldToCellXClamped(maxX);
	yEnd=WorldToCellYClamped(maxY);

	for (xCur=xStart; xCur <= xEnd; ++xCur)
	{
		for (yCur=yStart; yCur <= yEnd; ++yCur)
		{
			cell = grid+yCur*gridCellWidthCount+xCur;
			for (index=0; index < cell->Size(); ++index)
				intersectionList.Push(cell->operator [](index), _FILE_AND_LINE_);
		}
	}
}
bool PointGridSectorizer::HasEntry(void *entry)
{
	return entryRecords.Peek(entry)!=0;
}
unsigned int PointGridSectorizer::Size(void) const
{
	return entryRecords.Size();
}
void PointGridSectorizer::Clear(void)
{
	if (grid==0)
		return;
	int cur;
	int count = gridCellWidthCount*gridCellHeightCount;
	for (cur=0; cur<count; cur++)
		grid[cur].Clear(true, _FILE_AND_LINE_);
	entryRecords.Clear(_FILE_AND_LINE_);
}
void PointGridSectorizer::InsertIntoCell(void *entry, const int cellIndex)
{
	DataStructures::List<void*> &cell = grid[cellIndex];
	cell.Push(entry, _FILE_AND_LINE_);
	EntryRecord record;
	record.cellIndex=cellIndex;
	record.slotIndex=cell.Size()-1;
	entryRecords.Push(entry, record, _FILE_AND_LINE_);
}
void PointGridSectorizer::RemoveFromCell(const EntryRecord &record)
{
	DataStructures::List<void*> &cell = grid[record.cellIndex];
	const unsigned int lastSlot = cell.Size()-1;
	if (record.slotIndex != lastSlot)
	{
		// Swap-remove: the last entry fills the freed slot; fix its stored slot.
		void *moved = cell[lastSlot];
		cell[record.slotIndex]=moved;
		entryRecords.Peek(moved)->slotIndex=record.slotIndex;
	}
	cell.RemoveFromEnd();
}
int PointGridSectorizer::WorldToCellXClamped(const float input) const
{
	// Clamp in float space BEFORE the int cast: casting NaN or a value beyond
	// int range to int is undefined behavior (and saturates differently per
	// platform). The negated comparison sends NaN to cell 0.
	const float cell = (input-cellOriginX)*invCellWidth;
	if (!(cell > 0.0f))
		return 0;
	if (cell >= (float) gridCellWidthCount)
		return gridCellWidthCount-1;
	const int asInt = (int) cell;
	return asInt < gridCellWidthCount ? asInt : gridCellWidthCount-1;
}
int PointGridSectorizer::WorldToCellYClamped(const float input) const
{
	const float cell = (input-cellOriginY)*invCellHeight;
	if (!(cell > 0.0f))
		return 0;
	if (cell >= (float) gridCellHeightCount)
		return gridCellHeightCount-1;
	const int asInt = (int) cell;
	return asInt < gridCellHeightCount ? asInt : gridCellHeightCount-1;
}
int PointGridSectorizer::WorldToCellIndexClamped(const float x, const float y) const
{
	return WorldToCellYClamped(y)*gridCellWidthCount + WorldToCellXClamped(x);
}
