/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/tcp_interface.h"
#include "mafianet/plugin_interface2.h"

#if _RAKNET_SUPPORT_TCPInterface == 1

using namespace MafiaNet;

/*
Covers TCPInterface's plugin list, which has no tests.

The reason these exist: DetachPlugin looks the plugin up and acts on the result,
and the "not found" case is the one that goes wrong quietly. DataStructures::List
::GetIndexOf returned (unsigned)-1 when absent; std::find returns end(), and the
index form of that is size(), not -1. A detach of a plugin that was never attached
then indexes one past the end and swap-removes a plugin that is still attached --
with no size mismatch and no error to notice. So the cases below detach plugins
that are not in the list, and detach the same plugin twice.

Hermetic: Start(0, 0) creates no listen socket, and the plugin list is touched
only by the calling thread.
*/

namespace
{
	class CountingPlugin : public PluginInterface2
	{
	public:
		CountingPlugin() : attachCount(0), detachCount(0) {}

		void OnAttach(void) override { attachCount++; }
		void OnDetach(void) override { detachCount++; }

		int attachCount;
		int detachCount;
	};

	class TCPInterfacePlugins : public ::testing::Test
	{
	protected:
		void SetUp() override { ASSERT_TRUE(tcp.Start(0, 0)); }
		void TearDown() override { tcp.Stop(); }

		TCPInterface tcp;
	};
}

TEST_F(TCPInterfacePlugins, AttachThenDetachCallsEachHookOnce)
{
	CountingPlugin a;

	tcp.AttachPlugin(&a);
	ASSERT_EQ(a.attachCount, 1);
	ASSERT_EQ(a.detachCount, 0);

	tcp.DetachPlugin(&a);
	ASSERT_EQ(a.attachCount, 1);
	ASSERT_EQ(a.detachCount, 1);
}

TEST_F(TCPInterfacePlugins, AttachingTheSamePluginTwiceAttachesItOnce)
{
	CountingPlugin a;

	tcp.AttachPlugin(&a);
	tcp.AttachPlugin(&a);

	ASSERT_EQ(a.attachCount, 1) << "the second attach must be ignored";

	// And one detach is enough to remove it.
	tcp.DetachPlugin(&a);
	ASSERT_EQ(a.detachCount, 1);

	tcp.DetachPlugin(&a);
	ASSERT_EQ(a.detachCount, 1) << "the second detach must be a no-op";
}

// The case the index arithmetic gets wrong: detaching a plugin that was never
// attached must not touch the plugin that was.
TEST_F(TCPInterfacePlugins, DetachingAnUnattachedPluginLeavesTheAttachedOneAlone)
{
	CountingPlugin attached;
	CountingPlugin never;

	tcp.AttachPlugin(&attached);

	tcp.DetachPlugin(&never);

	ASSERT_EQ(never.detachCount, 0) << "a plugin that was never attached must not be detached";
	ASSERT_EQ(attached.detachCount, 0) << "the attached plugin must not have been detached";

	// If the previous call had swap-removed the attached plugin, this detach would
	// find nothing to do.
	tcp.DetachPlugin(&attached);
	ASSERT_EQ(attached.detachCount, 1) << "the attached plugin was no longer in the list";
}

TEST_F(TCPInterfacePlugins, DetachingFromAnEmptyListIsANoOp)
{
	CountingPlugin never;

	tcp.DetachPlugin(&never);

	ASSERT_EQ(never.attachCount, 0);
	ASSERT_EQ(never.detachCount, 0);
}

TEST_F(TCPInterfacePlugins, DetachingTheMiddleOfThreeKeepsTheOtherTwo)
{
	CountingPlugin a, b, c;

	tcp.AttachPlugin(&a);
	tcp.AttachPlugin(&b);
	tcp.AttachPlugin(&c);

	tcp.DetachPlugin(&b);

	ASSERT_EQ(b.detachCount, 1);
	ASSERT_EQ(a.detachCount, 0);
	ASSERT_EQ(c.detachCount, 0);

	// Both survivors must still be reachable -- the swap-remove moves c into b's
	// slot, and losing track of it here is the classic off-by-one.
	tcp.DetachPlugin(&a);
	tcp.DetachPlugin(&c);
	ASSERT_EQ(a.detachCount, 1) << "a was lost from the list";
	ASSERT_EQ(c.detachCount, 1) << "c was lost when it was swapped into b's slot";
}

TEST_F(TCPInterfacePlugins, DetachingNullIsIgnored)
{
	CountingPlugin a;
	tcp.AttachPlugin(&a);

	tcp.DetachPlugin(0);

	ASSERT_EQ(a.detachCount, 0);

	tcp.DetachPlugin(&a);
	ASSERT_EQ(a.detachCount, 1);
}

#endif // _RAKNET_SUPPORT_TCPInterface
