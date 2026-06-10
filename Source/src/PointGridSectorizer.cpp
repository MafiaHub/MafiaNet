/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/assert.h"
#include "mafianet/PointGridSectorizer.h"
#include <math.h>

using namespace MafiaNet;

PointGridSectorizer::PointGridSectorizer()
{
	grid=0;
	cellWidth=cellHeight=0.0f;
}
PointGridSectorizer::~PointGridSectorizer()
{
	if (grid)
		MafiaNet::OP_DELETE_ARRAY(grid, _FILE_AND_LINE_);
}
void PointGridSectorizer::Init(const float _cellWidth, const float _cellHeight, const float minX, const float minY, const float maxX, const float maxY)
{
	RakAssert(_cellWidth > 0.0f && _cellHeight > 0.0f);
	RakAssert(minX < maxX && minY < maxY);
	if (grid)
		MafiaNet::OP_DELETE_ARRAY(grid, _FILE_AND_LINE_);
	entryRecords.Clear(_FILE_AND_LINE_);

	cellOriginX=minX;
	cellOriginY=minY;
	gridWidth=maxX-minX;
	gridHeight=maxY-minY;
	gridCellWidthCount=(int) ceil(gridWidth/_cellWidth);
	gridCellHeightCount=(int) ceil(gridHeight/_cellHeight);
	// Make the cells slightly smaller, so we allocate an extra unneeded cell if on the edge.  This way we don't go outside the array on rounding errors.
	cellWidth=gridWidth/gridCellWidthCount;
	cellHeight=gridHeight/gridCellHeightCount;
	invCellWidth = 1.0f / cellWidth;
	invCellHeight = 1.0f / cellHeight;

	grid = MafiaNet::OP_NEW_ARRAY<DataStructures::List<void*> >(gridCellWidthCount*gridCellHeightCount, _FILE_AND_LINE_ );
}
void PointGridSectorizer::AddEntry(void *entry, const float x, const float y)
{
	// Same upsert as MoveEntry: one record per pointer, relocate if already present.
	MoveEntry(entry, x, y);
}
bool PointGridSectorizer::RemoveEntry(void *entry)
{
	RakAssert(cellWidth>0.0f);

	EntryRecord *record = entryRecords.Peek(entry);
	if (record==0)
		return false;
	RemoveFromCell(*record);
	entryRecords.Remove(entry, _FILE_AND_LINE_);
	return true;
}
void PointGridSectorizer::MoveEntry(void *entry, const float x, const float y)
{
	RakAssert(cellWidth>0.0f);

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
	RakAssert(cellWidth>0.0f);

	const DataStructures::List<void*> *cell;
	int xStart, yStart, xEnd, yEnd, xCur, yCur;
	unsigned index;
	xStart=WorldToCellXOffsetAndClamped(minX);
	yStart=WorldToCellYOffsetAndClamped(minY);
	xEnd=WorldToCellXOffsetAndClamped(maxX);
	yEnd=WorldToCellYOffsetAndClamped(maxY);

	intersectionList.Clear(true, _FILE_AND_LINE_);
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
int PointGridSectorizer::WorldToCellX(const float input) const
{
	return (int)((input-cellOriginX)*invCellWidth);
}
int PointGridSectorizer::WorldToCellY(const float input) const
{
	return (int)((input-cellOriginY)*invCellHeight);
}
int PointGridSectorizer::WorldToCellXOffsetAndClamped(const float input) const
{
	int cell=WorldToCellX(input);
	cell = cell > 0 ? cell : 0; // __max(cell,0);
	cell = gridCellWidthCount-1 < cell ? gridCellWidthCount-1 : cell; // __min(gridCellWidthCount-1, cell);
	return cell;
}
int PointGridSectorizer::WorldToCellYOffsetAndClamped(const float input) const
{
	int cell=WorldToCellY(input);
	cell = cell > 0 ? cell : 0; // __max(cell,0);
	cell = gridCellHeightCount-1 < cell ? gridCellHeightCount-1 : cell; // __min(gridCellHeightCount-1, cell);
	return cell;
}
int PointGridSectorizer::WorldToCellIndexClamped(const float x, const float y) const
{
	return WorldToCellYOffsetAndClamped(y)*gridCellWidthCount + WorldToCellXOffsetAndClamped(x);
}
