#include "mafianet/cc_sliding_window.h"

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
#include <gtest/gtest.h>

TEST(SlidingWindow, RetryAndNewPayloadSpendTheSameCredit)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, 1000);
	window.OnAck(100000, 100000, false, 0, 0, 0, true, 0);
	window.Update(100000, false);
	ASSERT_EQ(window.GetRetransmissionBandwidth(100000, 0, 5000, true), 1000);
	window.OnSendBytes(100000, 1200);
	EXPECT_EQ(window.GetTransmissionBandwidth(100000, 0, 0, true), 0);
	EXPECT_EQ(window.GetRetransmissionBandwidth(100000, 0, 5000, true), 0);
	window.Update(101000, true);
	EXPECT_EQ(window.GetTransmissionBandwidth(101000, 1000, 0, true), 0);
	window.Update(111000, true);
	EXPECT_GT(window.GetTransmissionBandwidth(111000, 10000, 0, true), 0);
	window.Update(900000, false);
	EXPECT_EQ(window.GetTransmissionBandwidth(900000, 789000, 0, false), 1000);
}

TEST(SlidingWindow, IsolatedRetryKeepsTheWindowWhileAckBlackoutRestartsSlowStart)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, 1000);
	for (unsigned i = 0; i < 10; ++i)
		window.OnAck(100000, 100000, false, 0, 0, 0, true, window.GetAndIncrementNextDatagramSequenceNumber());
	const auto before = window.GetCWNDLimit();
	ASSERT_GT(before, 2000u);
	window.OnNAK(101000, 5);
	window.OnResend(102000, 102000);
	EXPECT_EQ(window.GetCWNDLimit(), before);
	window.OnResend(600000, 600000);
	EXPECT_EQ(window.GetCWNDLimit(), 1000u);
}

TEST(SlidingWindow, FrequentGapsReduceWindowEvenWithAcksArriving)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, 1000);
	for (unsigned i = 0; i < 10; ++i)
		window.OnAck(100000, 100000, false, 0, 0, 0, true, window.GetAndIncrementNextDatagramSequenceNumber());
	for (unsigned gap = 0; gap < 3; ++gap)
		window.OnNAK(110000, gap);
	window.OnAck(130000, 100000, false, 0, 0, 0, true, window.GetAndIncrementNextDatagramSequenceNumber());
	window.OnResend(131000, 131000);
	EXPECT_EQ(window.GetCWNDLimit(), 1000u);
}

TEST(SlidingWindow, StandingDelayReducesTheWindowAndItGrowsAfterTheQueueDrains)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, 1000);
	auto ack = [&](CCTimeType time, CCTimeType rtt)
	{ window.OnAck(time, rtt, false, 0, 0, 0, true, window.GetAndIncrementNextDatagramSequenceNumber()); };
	for (unsigned i = 0; i < 15; ++i)
		ack(100000, 100000);
	// End slow start, then present two complete observation windows of inflated RTT.
	window.OnNAK(110000, 3);
	ack(120000, 180000);
	ack(180000, 180000);
	const auto before = window.GetCWNDLimit();
	ack(240000, 180000);
	ack(241000, 180000);
	EXPECT_LT(window.GetCWNDLimit(), before);
	const auto reduced = window.GetCWNDLimit();
	for (unsigned i = 0; i < 10; ++i)
		ack(300000 + i * 30000, 100000);
	EXPECT_GT(window.GetCWNDLimit(), reduced);
}
#endif
