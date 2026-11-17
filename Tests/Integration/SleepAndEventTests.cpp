/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/signaled_event.h"
#include "mafianet/sleep.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(SleepAndEvent, PositiveSleepLastsAtLeastTheRequestedDuration)
{
	const auto start = Clock::now();
	RakSleep(10);
	EXPECT_GE(Clock::now() - start, 10ms);
}

TEST(SleepAndEvent, RepeatedSignalsAreConsumedByOneWait)
{
	MafiaNet::SignaledEvent event;
	event.InitEvent();
	event.SetEvent();
	event.SetEvent();

	const auto start = Clock::now();
	event.WaitOnEvent(5000);
	EXPECT_LT(Clock::now() - start, 2s);

	const auto consumed = Clock::now();
	event.WaitOnEvent(20);
	EXPECT_GE(Clock::now() - consumed, 20ms);
	event.CloseEvent();
}

TEST(SleepAndEvent, ReopeningDiscardsAPendingSignal)
{
	MafiaNet::SignaledEvent event;
	event.InitEvent();
	event.SetEvent();
	event.CloseEvent();

	event.InitEvent();
	const auto start = Clock::now();
	event.WaitOnEvent(20);
	EXPECT_GE(Clock::now() - start, 20ms);
	event.CloseEvent();
}

TEST(SleepAndEvent, OneSignalReleasesOneOfTwoWaiters)
{
	MafiaNet::SignaledEvent event;
	event.InitEvent();

	// Each waiter has its own start time, so a late start cannot be mistaken for a timeout.
	auto wait = [&event](std::promise<void> &started)
	{
		const auto start = Clock::now();
		started.set_value();
		event.WaitOnEvent(1000);
		return Clock::now() - start;
	};
	std::promise<void> firstStarted, secondStarted;
	auto first = std::async(std::launch::async, wait, std::ref(firstStarted));
	auto second = std::async(std::launch::async, wait, std::ref(secondStarted));
	firstStarted.get_future().wait();
	secondStarted.get_future().wait();

	event.SetEvent();
	const bool firstTimedOut = first.get() >= 1s;
	const bool secondTimedOut = second.get() >= 1s;
	EXPECT_NE(firstTimedOut, secondTimedOut);
	event.CloseEvent();
}

TEST(SleepAndEvent, IndefiniteWaitEndsOnSignal)
{
	MafiaNet::SignaledEvent event;
	event.InitEvent();

	std::promise<void> started;
	auto waitForSignal = [&]
	{
		started.set_value();
		event.WaitOnEvent(-1);
	};
	auto waiter = std::async(std::launch::async, waitForSignal);
	started.get_future().wait();
	EXPECT_EQ(waiter.wait_for(20ms), std::future_status::timeout);
	event.SetEvent();
	waiter.get();
	event.CloseEvent();
}
