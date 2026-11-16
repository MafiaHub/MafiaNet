/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "SQLite3PluginCommon.h"


SQLite3Table::SQLite3Table()
{
}
SQLite3Table::~SQLite3Table()
{
}

void SQLite3Table::Serialize(MafiaNet::BitStream *bitStream)
{
	bitStream->Write((unsigned int) columnNames.size());
	unsigned int idx1, idx2;
	for (idx1 = 0; idx1 < columnNames.size(); idx1++)
		bitStream->Write(columnNames[idx1]);
	bitStream->Write((unsigned int) rows.size());
	for (idx1 = 0; idx1 < rows.size(); idx1++)
	{
		for (idx2 = 0; idx2 < rows[idx1]->entries.size(); idx2++)
		{
			bitStream->Write(rows[idx1]->entries[idx2]);
		}
	}
}
void SQLite3Table::Deserialize(MafiaNet::BitStream *bitStream)
{
	rows.clear();
	columnNames.clear();

	unsigned int numColumns, numRows;
	bitStream->Read(numColumns);
	unsigned int idx1, idx2;
	MafiaNet::RakString inputStr;
	for (idx1 = 0; idx1 < numColumns; idx1++)
	{
		bitStream->Read(inputStr);
		columnNames.push_back(inputStr);
	}
	bitStream->Read(numRows);
	for (idx1 = 0; idx1 < numRows; idx1++)
	{
		rows.push_back(std::make_unique<SQLite3Row>());
		SQLite3Row *row = rows.back().get();
		for (idx2 = 0; idx2 < numColumns; idx2++)
		{
			bitStream->Read(inputStr);
			row->entries.push_back(inputStr);
		}
	}
}
