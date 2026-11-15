/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/connect_candidates.h"

#include <vector>

namespace MafiaNet
{

int OrderConnectCandidates(SystemAddress *addresses, int count)
{
	if (addresses == nullptr || count <= 0)
		return 0;

	std::vector<SystemAddress> first, other;
	first.reserve((size_t) count);
	other.reserve((size_t) count);
	const unsigned char firstFamily = addresses[0].GetIPVersion();
	for (int i = 0; i < count; ++i)
	{
		bool duplicate = false;
		for (int j = 0; j < i && !duplicate; ++j)
			duplicate = addresses[j] == addresses[i];
		if (duplicate)
			continue;
		(addresses[i].GetIPVersion() == firstFamily ? first : other).push_back(addresses[i]);
	}

	int out = 0;
	size_t a = 0, b = 0;
	while (a < first.size() || b < other.size())
	{
		if (a < first.size())
			addresses[out++] = first[a++];
		if (b < other.size())
			addresses[out++] = other[b++];
	}
	return out;
}

} // namespace MafiaNet
