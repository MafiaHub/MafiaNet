/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Fuzzes TableSerializer deserialization of tables and filter queries.
 *  The first byte picks the entry point; the rest is the
 *  serialized data. Whatever deserializes is then used the way a receiver uses it: tables are
 *  sorted and re-serialized, filter queries are run against a fixed table.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"
#include "mafianet/table.h"
#include "mafianet/table_serializer.h"

using namespace MafiaNet;

namespace
{
void BuildQueryTarget(Table &t)
{
	t.AddColumn("id", Table::NUMERIC);
	t.AddColumn("name", Table::STRING);
	t.AddColumn("blob", Table::BINARY);
	t.AddColumn("ptr", Table::POINTER);
	Table::Row *row = t.AddRow(1);
	row->cells[0]->Set(1);
	row->cells[1]->Set("one");
	row->cells[2]->Set("\x01\x02", 2);
	row->cells[3]->SetPtr(row);
	t.AddRow(2)->cells[1]->Set("two");
}

void UseTable(Table &t)
{
	std::vector<Table::SortQuery> sorts;
	for (unsigned i = 0; i < t.GetColumnCount() && i < 4; i++)
		sorts.push_back({i, (i & 1) ? Table::QS_DECREASING_ORDER : Table::QS_INCREASING_ORDER});
	std::vector<Table::Row *> sorted(t.GetRowCount());
	if (!sorts.empty())
		t.SortTable(sorts.data(), (unsigned) sorts.size(), sorted.data());

	BitStream out;
	TableSerializer::SerializeTable(&t, &out);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const bool compressorReady = (StringCompressor::AddReference(), true);
	(void) compressorReady;

	if (size == 0)
		return 0;
	BitStream in(const_cast<unsigned char *>(data + 1), (unsigned int) (size - 1), false);

	switch (data[0] % 3)
	{
	case 0:
	{
		Table t;
		if (TableSerializer::DeserializeTable(&in, &t))
			UseTable(t);
		break;
	}
	case 1:
	{
		Table::FilterQuery *queries = nullptr;
		unsigned int numQueries = 0;
		const bool ok = TableSerializer::DeserializeFilterQueryList(&in, &queries, &numQueries, 8);
		if (ok && numQueries > 0)
		{
			Table target, result;
			BuildQueryTarget(target);
			target.QueryTable(nullptr, 0, queries, numQueries, nullptr, 0, &result);
			UseTable(result);
		}
		TableSerializer::DeallocateQueryList(queries, numQueries);
		break;
	}
	case 2:
	{
		// A row update against a table whose columns the receiver already has.
		Table t;
		BuildQueryTarget(t);
		t.RemoveRow(1);
		if (TableSerializer::DeserializeRow(&in, &t))
			UseTable(t);
		break;
	}
	}
	return 0;
}
