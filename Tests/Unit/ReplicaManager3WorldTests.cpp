/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/replica_manager3.h"

using namespace MafiaNet;

namespace
{
	class WorldTestConnection : public Connection_RM3
	{
	public:
		WorldTestConnection(const SystemAddress &sa, RakNetGUID g) : Connection_RM3(sa, g) {}
		Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
	};

	class WorldTestManager : public ReplicaManager3
	{
	public:
		Connection_RM3 *AllocConnection(const SystemAddress &sa, RakNetGUID g) const { return new WorldTestConnection(sa, g); }
		void DeallocConnection(Connection_RM3 *connection) const { delete connection; }
	};
}

TEST(ReplicaManager3Worlds, DefaultWorldIsCounted)
{
	WorldTestManager manager;
	ASSERT_EQ(manager.GetWorldCount(), 1u);
	EXPECT_EQ(manager.GetWorldIdAtIndex(0), static_cast<WorldId>(0));
}

// WorldId is a byte; every value it can take, 255 included, has a slot in the world table.
TEST(ReplicaManager3Worlds, HighestWorldIdIsUsable)
{
	WorldTestManager manager;
	manager.AddWorld(255);
	ASSERT_EQ(manager.GetWorldCount(), 2u) << "World 0 plus world 255";
	EXPECT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(255));
	manager.RemoveWorld(255);
	EXPECT_EQ(manager.GetWorldCount(), 1u);
}
