/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/instance_ptr.h"
#include "mafianet/message_filter.h"

namespace
{
// Counts factory traffic so the deleter's behaviour is observable.
struct CountedThing
{
	static int created;
	static int destroyed;
	static CountedThing *GetInstance()
	{
		++created;
		return new CountedThing;
	}
	static void DestroyInstance(CountedThing *p)
	{
		++destroyed;
		delete p;
	}
};
int CountedThing::created = 0;
int CountedThing::destroyed = 0;

class InstancePtrCounts : public ::testing::Test
{
protected:
	void SetUp() override
	{
		CountedThing::created = 0;
		CountedThing::destroyed = 0;
	}
};
} // namespace

TEST(InstancePtr, DestroysThroughDestroyInstance)
{
	auto p = MafiaNet::MakeInstance<MafiaNet::MessageFilter>();
	EXPECT_NE(p.get(), nullptr);
	p.reset(); // a sanitizer reports a leak or mismatched free if the deleter is wrong
	EXPECT_EQ(p.get(), nullptr);
}

TEST_F(InstancePtrCounts, DestroyInstanceCalledExactlyOnce)
{
	auto p = MafiaNet::MakeInstance<CountedThing>();
	ASSERT_NE(p.get(), nullptr);
	EXPECT_EQ(CountedThing::created, 1);
	EXPECT_EQ(CountedThing::destroyed, 0);
	p.reset();
	EXPECT_EQ(CountedThing::destroyed, 1);
	p.reset();
	EXPECT_EQ(CountedThing::destroyed, 1);
}

TEST_F(InstancePtrCounts, NullPointerDoesNotCallDestroyInstance)
{
	{
		MafiaNet::InstancePtr<CountedThing> p;
		p.reset();
	}
	EXPECT_EQ(CountedThing::destroyed, 0);
}

TEST_F(InstancePtrCounts, DestructorDestroysOwnedInstance)
{
	{
		auto p = MafiaNet::MakeInstance<CountedThing>();
	}
	EXPECT_EQ(CountedThing::destroyed, 1);
}
