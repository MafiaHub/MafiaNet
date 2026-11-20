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
