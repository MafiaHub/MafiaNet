#include "mafianet/cc_sliding_window.h"

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
#include <gtest/gtest.h>

TEST(SlidingWindow, CongestionAvoidanceRestoresOneDatagramPerAcknowledgedFlight)
{
	constexpr unsigned mtu = 1000;
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, mtu);
	for (unsigned i = 0; i < 8; ++i)
		window.GetAndIncrementNextDatagramSequenceNumber();
	for (unsigned i = 0; i < 8; ++i)
		window.OnAck(100000, 100000, false, 0, 0, 0, true, i);
	window.OnNAK(100000, 7);
	ASSERT_FALSE(window.GetIsInSlowStart());

	for (unsigned flight = 0; flight < 3; ++flight)
	{
		const int before = window.GetTransmissionBandwidth(0, 0, 0, true);
		const auto first = window.GetNextDatagramSequenceNumber();
		for (int i = 0; i < before / static_cast<int>(mtu); ++i)
			window.GetAndIncrementNextDatagramSequenceNumber();
		for (auto ack = first; ack != window.GetNextDatagramSequenceNumber(); ++ack)
			window.OnAck(200000 + flight * 100000, 100000, false, 0, 0, 0, true, ack);
		EXPECT_EQ(window.GetTransmissionBandwidth(0, 0, 0, true), before + static_cast<int>(mtu));
	}
}
#endif

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
TEST(SlidingWindow, ReservedFlightNeedsRecentAcknowledgedCapacity)
{
	MafiaNet::CCRakNetSlidingWindow window;
	constexpr unsigned mtu = 1000;
	constexpr unsigned rate = 800000; // 100 kB/s
	window.Init(0, mtu);
	window.UpdateWindowFloor(0, rate);
	EXPECT_EQ(window.GetCWNDLimit(), mtu);
	window.OnAck(100000, 100000, false, 0, 0, 0, false, 0);
	window.OnAck(300000, 200000, false, 0, 0, 0, false, 1);
	window.UpdateWindowFloor(300000, rate);
	EXPECT_EQ(window.GetCWNDLimit(), 11000u);
	EXPECT_EQ(window.GetTransmissionBandwidth(300000, 0, 10000, true), 1000);
	window.UpdateWindowFloor(300000 + window.GetRTOForRetransmission(1) + 1, rate);
	EXPECT_EQ(window.GetCWNDLimit(), mtu);

	// Old samples age out after an extended gap, and even an extreme RTT can
	// grant at most one second of the configured rate.
	window.OnAck(5000000, 4000000, false, 0, 0, 0, false, 2);
	window.UpdateWindowFloor(5000000, UINT32_MAX);
	EXPECT_EQ(window.GetCWNDLimit(), UINT32_MAX / 8u);
	window.UpdateWindowFloor(5000000, 0);
	EXPECT_EQ(window.GetCWNDLimit(), mtu);
	window.UpdateWindowFloor(5000000, rate);
	EXPECT_EQ(window.GetCWNDLimit(), mtu);
}
#endif
