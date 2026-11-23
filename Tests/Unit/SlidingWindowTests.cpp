#include "mafianet/cc_sliding_window.h"

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
#include <gtest/gtest.h>

#include <cstdint>

namespace
{
// RakNet's classic default datagram payload size.
constexpr uint32_t kMtu = 1347;

// Acknowledge count datagrams starting at seq, as a continuous sender whose
// datagram sequence counter never advances. Every ack past the first then
// opens a new congestion-control period, so in slow start each ack grows the
// window by one datagram. Real flights open one period per acknowledged
// flight; that growth pattern is covered by
// CongestionAvoidanceRestoresOneDatagramPerAcknowledgedFlight below.
void Ack(MafiaNet::CCRakNetSlidingWindow &window, DatagramSequenceNumberType seq, int count, CCTimeType now = 0)
{
	for (int i = 0; i < count; ++i)
	{
		window.OnAck(now, 100000, false, 0, 0, 0, true, seq);
		seq = seq + (DatagramSequenceNumberType) 1;
	}
}
} // namespace

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
		const uint32_t before = window.GetCWNDLimit();
		const auto first = window.GetNextDatagramSequenceNumber();
		for (uint32_t i = 0; i < before / mtu; ++i)
			window.GetAndIncrementNextDatagramSequenceNumber();
		for (auto ack = first; ack != window.GetNextDatagramSequenceNumber(); ++ack)
			window.OnAck(200000 + flight * 100000, 100000, false, 0, 0, 0, true, ack);
		EXPECT_EQ(window.GetCWNDLimit(), before + mtu);
	}
}

TEST(SlidingWindow, SlowStartGrowsWindowByOneDatagramPerAck)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, kMtu);
	EXPECT_EQ(window.GetCWNDLimit(), kMtu);
	EXPECT_TRUE(window.GetIsInSlowStart());
	Ack(window, 0, 20);
	EXPECT_EQ(window.GetCWNDLimit(), 21 * kMtu);
	EXPECT_TRUE(window.GetIsInSlowStart());
}

TEST(SlidingWindow, NakEndsSlowStartWithoutShrinkingWindow)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, kMtu);
	Ack(window, 0, 10);
	EXPECT_EQ(window.GetCWNDLimit(), 11 * kMtu);
	EXPECT_TRUE(window.GetIsInSlowStart());
	window.OnNAK(0, 0);
	// The NAK halves the slow-start threshold; the window itself is untouched
	// and further growth follows congestion avoidance.
	EXPECT_EQ(window.GetCWNDLimit(), 11 * kMtu);
	EXPECT_FALSE(window.GetIsInSlowStart());
}

TEST(SlidingWindow, TimeoutResendCollapsesWindowToOneDatagramAndReentersSlowStart)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, kMtu);
	Ack(window, 0, 10);
	EXPECT_EQ(window.GetCWNDLimit(), 11 * kMtu);
	const auto timeout = window.GetRTOForRetransmission(1);
	window.OnResend(timeout, timeout);
	// A timeout retransmission backs the window off to one datagram and re-enters
	// slow start against the halved threshold.
	EXPECT_EQ(window.GetCWNDLimit(), kMtu);
	EXPECT_TRUE(window.GetIsInSlowStart());
	// The collapsed window climbs back past its pre-collapse size.
	Ack(window, 10, 20, timeout + 100000);
	EXPECT_GT(window.GetCWNDLimit(), 11 * kMtu);
}

TEST(SlidingWindow, RepeatedTimeoutResendsRatchetTheWindowDown)
{
	MafiaNet::CCRakNetSlidingWindow window;
	window.Init(0, kMtu);
	DatagramSequenceNumberType seq = 0;
	uint32_t previousPeak = UINT32_MAX;
	CCTimeType now = 0;
	for (int round = 0; round < 8; ++round)
	{
		now += 100000;
		Ack(window, seq, 8, now);
		seq = seq + (DatagramSequenceNumberType) 8;
		// The window always rebuilds above the two-datagram backoff floor...
		EXPECT_GT(window.GetCWNDLimit(), 2 * kMtu);
		// ...yet every rebuild peaks lower than the one before it: each
		// collapse halves the threshold the rebuild clamps against.
		ASSERT_LT(window.GetCWNDLimit(), previousPeak);
		previousPeak = window.GetCWNDLimit();
		now += window.GetRTOForRetransmission(1);
		window.OnResend(now, now);
		EXPECT_EQ(window.GetCWNDLimit(), kMtu);
		EXPECT_TRUE(window.GetIsInSlowStart());
	}
}
#endif
