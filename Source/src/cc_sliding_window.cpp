/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/cc_sliding_window.h"

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1

static const double UNSET_TIME_US = -1;

#if CC_TIME_TYPE_BYTES == 4
static const CCTimeType SYN = 10;
#else
static const CCTimeType SYN = 10000;
#endif

#include "mafianet/mtu_size.h"
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <stdlib.h>
#include "mafianet/assert.h"

using namespace MafiaNet;

// ****************************************************** PUBLIC METHODS ******************************************************

CCRakNetSlidingWindow::CCRakNetSlidingWindow()
{
}
// ----------------------------------------------------------------------------------------------------------------------------
CCRakNetSlidingWindow::~CCRakNetSlidingWindow()
{
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::Init(CCTimeType curTime, uint32_t maxDatagramPayload)
{
	(void) curTime;

	pacingClock = baselineEpoch = recentEpoch = lastDelayBackoff = lastAckAt = curTime;
	intervalAcks = intervalGaps = 0;
	excessiveGaps = false;
	recentMinimum = completedMinimum = UNSET_TIME_US;
	previousMinRtt = currentMinRtt = UNSET_TIME_US;
	pacingCredit = maxDatagramPayload;
	lastRtt = estimatedRTT = deviationRtt = UNSET_TIME_US;
	RakAssert(maxDatagramPayload <= MAXIMUM_MTU_SIZE);
	MAXIMUM_MTU_INCLUDING_UDP_HEADER = maxDatagramPayload;
	cwnd = maxDatagramPayload;
	ssThresh = 0.0;
	oldestUnsentAck = 0;
	nextDatagramSequenceNumber = 0;
	nextCongestionControlBlock = 0;
	backoffThisBlock = speedUpThisBlock = false;
	expectedNextSequenceNumber = 0;
	_isContinuousSend = false;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::Update(CCTimeType curTime, bool hasDataToSendOrResend)
{
	const CCTimeType elapsed = curTime - pacingClock;
	pacingClock = curTime;
	if (estimatedRTT <= 0 || !hasDataToSendOrResend)
	{
		pacingCredit = MAXIMUM_MTU_INCLUDING_UDP_HEADER;
		return;
	}
	const double rate = (IsInSlowStart() ? 2.0 : 1.25) * cwnd / estimatedRTT;
	const double accrued = rate * std::min(elapsed, SYN * 3);
	const double burst = std::max(2.0 * MAXIMUM_MTU_INCLUDING_UDP_HEADER, accrued);
	pacingCredit = std::min(burst, pacingCredit + accrued);
}
// ----------------------------------------------------------------------------------------------------------------------------
int CCRakNetSlidingWindow::GetRetransmissionBandwidth(CCTimeType curTime, CCTimeType timeSinceLastTick, uint32_t unacknowledgedBytes, bool isContinuousSend)
{
	(void) curTime;
	(void) isContinuousSend;
	(void) timeSinceLastTick;

	return static_cast<int>(std::min<double>(unacknowledgedBytes, std::max(0.0, pacingCredit)));
}
// ----------------------------------------------------------------------------------------------------------------------------
int CCRakNetSlidingWindow::GetTransmissionBandwidth(CCTimeType curTime, CCTimeType timeSinceLastTick, uint32_t unacknowledgedBytes, bool isContinuousSend)
{
	(void) curTime;
	(void) timeSinceLastTick;

	_isContinuousSend = isContinuousSend;

	if (unacknowledgedBytes <= cwnd)
		return static_cast<int>(std::min(cwnd - unacknowledgedBytes, std::max(0.0, pacingCredit)));
	else
		return 0;
}
// ----------------------------------------------------------------------------------------------------------------------------
bool CCRakNetSlidingWindow::ShouldSendACKs(CCTimeType curTime, CCTimeType estimatedTimeToNextTick)
{
	CCTimeType rto = GetSenderRTOForACK();
	(void) estimatedTimeToNextTick;

	// UNSET_TIME_US is a negative double (-1); converting it straight to the unsigned
	// CCTimeType is undefined behavior, which Apple clang at -O2 turns into a trap
	// (brk #1). Cast through int64_t so the negative->unsigned conversion is defined
	// and yields the same sentinel as GetSenderRTOForACK() on every platform.
	if (rto == (CCTimeType) (int64_t) UNSET_TIME_US)
	{
		// Unknown how long until the remote system will retransmit, so better send right away
		return true;
	}

	return curTime >= oldestUnsentAck + SYN;
}
// ----------------------------------------------------------------------------------------------------------------------------
DatagramSequenceNumberType CCRakNetSlidingWindow::GetNextDatagramSequenceNumber(void)
{
	return nextDatagramSequenceNumber;
}
// ----------------------------------------------------------------------------------------------------------------------------
DatagramSequenceNumberType CCRakNetSlidingWindow::GetAndIncrementNextDatagramSequenceNumber(void)
{
	DatagramSequenceNumberType dsnt = nextDatagramSequenceNumber;
	nextDatagramSequenceNumber++;
	return dsnt;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnSendBytes(CCTimeType curTime, uint32_t numBytes)
{
	(void) curTime;
	pacingCredit -= numBytes;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnGotPacketPair(DatagramSequenceNumberType datagramSequenceNumber, uint32_t sizeInBytes, CCTimeType curTime)
{
	(void) curTime;
	(void) sizeInBytes;
	(void) datagramSequenceNumber;
}
// ----------------------------------------------------------------------------------------------------------------------------
bool CCRakNetSlidingWindow::OnGotPacket(DatagramSequenceNumberType datagramSequenceNumber, bool isContinuousSend, CCTimeType curTime, uint32_t sizeInBytes, uint32_t *skippedMessageCount)
{
	(void) curTime;
	(void) sizeInBytes;
	(void) isContinuousSend;

	if (oldestUnsentAck == 0)
		oldestUnsentAck = curTime;

	if (datagramSequenceNumber == expectedNextSequenceNumber)
	{
		*skippedMessageCount = 0;
		expectedNextSequenceNumber = datagramSequenceNumber + (DatagramSequenceNumberType) 1;
	}
	else if (GreaterThan(datagramSequenceNumber, expectedNextSequenceNumber))
	{
		*skippedMessageCount = datagramSequenceNumber - expectedNextSequenceNumber;
		// Sanity check, just use timeout resend if this was really valid
		if (*skippedMessageCount > 1000)
		{
			// During testing, the nat punchthrough server got 51200 on the first packet. I have no idea where this comes from, but has happened twice
			if (*skippedMessageCount > (uint32_t) 50000)
				return false;
			*skippedMessageCount = 1000;
		}
		expectedNextSequenceNumber = datagramSequenceNumber + (DatagramSequenceNumberType) 1;
	}
	else
	{
		*skippedMessageCount = 0;
	}

	return true;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnResend(CCTimeType curTime, MafiaNet::TimeUS nextActionTime)
{
	(void) nextActionTime;
	const double minimumWindow = MAXIMUM_MTU_INCLUDING_UDP_HEADER;
	if (!_isContinuousSend || backoffThisBlock || cwnd <= 2 * minimumWindow)
		return;

	const bool recentAck = estimatedRTT > 0 && curTime - lastAckAt < 2 * estimatedRTT + 2 * SYN;
	if (recentAck && !excessiveGaps)
		return;

	// Frequent gaps with continuing ACKs still leave a working path. Reserve
	// the one-datagram restart for a flight without recent ACK feedback.
	ssThresh = cwnd / 2;
	cwnd = recentAck ? ssThresh : minimumWindow;
	nextCongestionControlBlock = nextDatagramSequenceNumber;
	backoffThisBlock = true;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnNAK(CCTimeType curTime, DatagramSequenceNumberType nakSequenceNumber)
{
	(void) nakSequenceNumber;
	(void) curTime;

	++intervalGaps;
	if (_isContinuousSend && backoffThisBlock == false)
	{
		// Start congestion avoidance
		ssThresh = cwnd / 2;

		// CC PRINTF
		//printf("- %.0f (NAK) Set congestion avoidance.\n", cwnd);
	}
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnAck(CCTimeType curTime, CCTimeType rtt, bool hasBAndAS, BytesPerMicrosecond _B, BytesPerMicrosecond _AS, double totalUserDataBytesAcked, bool isContinuousSend, DatagramSequenceNumberType sequenceNumber)
{
	(void) _B;
	(void) totalUserDataBytesAcked;
	(void) _AS;
	(void) hasBAndAS;
	lastRtt = static_cast<double>(rtt);

	// Remember the minima of two ten-second intervals, to age out a changed path.
	if (curTime - baselineEpoch >= 1000 * SYN)
	{
		previousMinRtt = curTime - baselineEpoch < 2000 * SYN ? currentMinRtt : UNSET_TIME_US;
		currentMinRtt = UNSET_TIME_US;
		baselineEpoch = curTime;
	}
	if (currentMinRtt < 0 || lastRtt < currentMinRtt)
		currentMinRtt = lastRtt;
	const CCTimeType observation = 12 * SYN;
	lastAckAt = curTime;
	++intervalAcks;
	if (curTime - recentEpoch >= observation)
	{
		excessiveGaps = intervalGaps >= 3 && intervalGaps * 8 > intervalAcks;
		intervalAcks = intervalGaps = 0;
		completedMinimum = curTime - recentEpoch < 2 * observation ? recentMinimum : UNSET_TIME_US;
		recentMinimum = UNSET_TIME_US;
		recentEpoch = curTime;
	}
	if (recentMinimum < 0 || lastRtt < recentMinimum)
		recentMinimum = lastRtt;
	if (estimatedRTT == UNSET_TIME_US)
	{
		estimatedRTT = lastRtt;
		deviationRtt = lastRtt;
	}
	else
	{
		double d = .05;
		double difference = lastRtt - estimatedRTT;
		estimatedRTT = estimatedRTT + d * difference;
		deviationRtt = deviationRtt + d * (std::abs(difference) - deviationRtt);
	}

	_isContinuousSend = isContinuousSend;

	if (isContinuousSend == false)
		return;

	bool isNewCongestionControlPeriod;
	isNewCongestionControlPeriod = GreaterThan(sequenceNumber, nextCongestionControlBlock);

	if (isNewCongestionControlPeriod)
	{
		backoffThisBlock = false;
		speedUpThisBlock = false;
		nextCongestionControlBlock = nextDatagramSequenceNumber;
	}

	const double baseRtt = previousMinRtt < 0 ? currentMinRtt : std::min(previousMinRtt, currentMinRtt);
	const double delayTarget = std::clamp(baseRtt * 0.25, static_cast<double>(SYN), 3.0 * SYN);
	if (IsInSlowStart() && cwnd >= 8.0 * MAXIMUM_MTU_INCLUDING_UDP_HEADER && estimatedRTT - baseRtt > delayTarget)
		ssThresh = cwnd;
	if (completedMinimum - baseRtt > delayTarget && recentMinimum - baseRtt > delayTarget &&
	    cwnd <= 2.0 * MAXIMUM_MTU_INCLUDING_UDP_HEADER && curTime - lastDelayBackoff >= 2 * observation)
	{
		// A window drained down to two packets still sees this delay: a slower route or another flow.
		previousMinRtt = UNSET_TIME_US;
		currentMinRtt = std::min(completedMinimum, recentMinimum);
		baselineEpoch = curTime;
	}
	else if (completedMinimum - baseRtt > delayTarget && recentMinimum - baseRtt > delayTarget)
	{
		if (isNewCongestionControlPeriod && curTime - lastDelayBackoff >= observation)
		{
			if (cwnd > 2.0 * MAXIMUM_MTU_INCLUDING_UDP_HEADER)
				lastDelayBackoff = curTime;
			cwnd = std::max(2.0 * MAXIMUM_MTU_INCLUDING_UDP_HEADER, 0.8 * cwnd);
			ssThresh = cwnd;
			backoffThisBlock = true;
		}
		return;
	}
	if (IsInSlowStart())
	{
		cwnd += MAXIMUM_MTU_INCLUDING_UDP_HEADER;
		if (cwnd > ssThresh && ssThresh != 0)
			cwnd = ssThresh + MAXIMUM_MTU_INCLUDING_UDP_HEADER * MAXIMUM_MTU_INCLUDING_UDP_HEADER / cwnd;

		// CC PRINTF
		//	printf("++ %.0f Slow start increase.\n", cwnd);
	}
	else if (isNewCongestionControlPeriod)
	{
		// Only one ACK advances the window per flight. Add one datagram here;
		// MTU * MTU / cwnd is the increment for an ACK of EACH datagram.
		cwnd += MAXIMUM_MTU_INCLUDING_UDP_HEADER;

		// CC PRINTF
		// printf("+ %.0f Congestion avoidance increase.\n", cwnd);
	}
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnDuplicateAck(CCTimeType curTime, DatagramSequenceNumberType sequenceNumber)
{
	(void) curTime;
	(void) sequenceNumber;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnSendAckGetBAndAS(CCTimeType curTime, bool *hasBAndAS, BytesPerMicrosecond *_B, BytesPerMicrosecond *_AS)
{
	(void) curTime;
	(void) _B;
	(void) _AS;

	*hasBAndAS = false;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnSendAck(CCTimeType curTime, uint32_t numBytes)
{
	(void) curTime;
	(void) numBytes;

	oldestUnsentAck = 0;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::OnSendNACK(CCTimeType curTime, uint32_t numBytes)
{
	(void) curTime;
	(void) numBytes;
}
// ----------------------------------------------------------------------------------------------------------------------------
CCTimeType CCRakNetSlidingWindow::GetRTOForRetransmission(unsigned char timesSent) const
{
	(void) timesSent;

#if CC_TIME_TYPE_BYTES == 4
	const CCTimeType maxThreshold = 2000;
	//const CCTimeType minThreshold=100;
	const CCTimeType additionalVariance = 30;
#else
	const CCTimeType maxThreshold = 2000000;
	//const CCTimeType minThreshold=100000;
	const CCTimeType additionalVariance = 30000;
#endif


	if (estimatedRTT == UNSET_TIME_US)
		return maxThreshold;

	//double u=1.0f;
	double u = 2.0f;
	double q = 4.0f;

	CCTimeType threshhold = (CCTimeType) (u * estimatedRTT + q * deviationRtt) + additionalVariance;
	if (threshhold > maxThreshold)
		return maxThreshold;
	return threshhold;
}
// ----------------------------------------------------------------------------------------------------------------------------
void CCRakNetSlidingWindow::SetMTU(uint32_t bytes)
{
	RakAssert(bytes < MAXIMUM_MTU_SIZE);
	MAXIMUM_MTU_INCLUDING_UDP_HEADER = bytes;
}
// ----------------------------------------------------------------------------------------------------------------------------
uint32_t CCRakNetSlidingWindow::GetMTU(void) const
{
	return MAXIMUM_MTU_INCLUDING_UDP_HEADER;
}
// ----------------------------------------------------------------------------------------------------------------------------
BytesPerMicrosecond CCRakNetSlidingWindow::GetLocalReceiveRate(CCTimeType currentTime) const
{
	(void) currentTime;

	return 0; // TODO
}
// ----------------------------------------------------------------------------------------------------------------------------
double CCRakNetSlidingWindow::GetRTT(void) const
{
	if (lastRtt == UNSET_TIME_US)
		return 0.0;
	return lastRtt;
}
// ----------------------------------------------------------------------------------------------------------------------------
bool CCRakNetSlidingWindow::GreaterThan(DatagramSequenceNumberType a, DatagramSequenceNumberType b)
{
	// a > b?
	const DatagramSequenceNumberType halfSpan = (DatagramSequenceNumberType) (((DatagramSequenceNumberType) (uint32_t) -1) / (DatagramSequenceNumberType) 2);
	return b != a && b - a > halfSpan;
}
// ----------------------------------------------------------------------------------------------------------------------------
bool CCRakNetSlidingWindow::LessThan(DatagramSequenceNumberType a, DatagramSequenceNumberType b)
{
	// a < b?
	const DatagramSequenceNumberType halfSpan = ((DatagramSequenceNumberType) (uint32_t) -1) / (DatagramSequenceNumberType) 2;
	return b != a && b - a < halfSpan;
}
// ----------------------------------------------------------------------------------------------------------------------------
uint64_t CCRakNetSlidingWindow::GetBytesPerSecondLimitByCongestionControl(void) const
{
	return 0; // TODO
}
// ----------------------------------------------------------------------------------------------------------------------------
CCTimeType CCRakNetSlidingWindow::GetSenderRTOForACK(void) const
{
	if (lastRtt == UNSET_TIME_US)
		return (CCTimeType) (int64_t) UNSET_TIME_US; // see ShouldSendACKs: avoid UB negative double->unsigned cast
	return (CCTimeType) (lastRtt + SYN);
}
// ----------------------------------------------------------------------------------------------------------------------------
bool CCRakNetSlidingWindow::IsInSlowStart(void) const
{
	return cwnd <= ssThresh || ssThresh == 0;
}
// ----------------------------------------------------------------------------------------------------------------------------
#endif
