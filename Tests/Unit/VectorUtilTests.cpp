/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/vector_util.h"

#include <memory>
#include <vector>

TEST(VectorUtil, SwapRemoveMovesLastIntoHole)
{
	std::vector<int> v{1, 2, 3, 4};
	MafiaNet::SwapRemove(v, 1);
	EXPECT_EQ(v, (std::vector<int>{1, 4, 3}));
}

TEST(VectorUtil, SwapRemoveOfLastElementJustPops)
{
	std::vector<int> v{1, 2, 3};
	MafiaNet::SwapRemove(v, 2);
	EXPECT_EQ(v, (std::vector<int>{1, 2}));
}

TEST(VectorUtil, SwapRemoveOfOnlyElementEmptiesVector)
{
	std::vector<int> v{7};
	MafiaNet::SwapRemove(v, 0);
	EXPECT_TRUE(v.empty());
}

TEST(VectorUtil, SwapRemoveWorksWithMoveOnlyTypes)
{
	std::vector<std::unique_ptr<int>> v;
	v.push_back(std::make_unique<int>(1));
	v.push_back(std::make_unique<int>(2));
	MafiaNet::SwapRemove(v, 0);
	ASSERT_EQ(v.size(), 1u);
	EXPECT_EQ(*v[0], 2);
}
