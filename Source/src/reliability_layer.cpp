/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
///


#include "mafianet/reliability_layer.h"
#include <vector>
#include <cstdlib>
#include "mafianet/mmsg_batch.h"
#include "mafianet/mtu_black_hole.h"
#include "mafianet/get_time.h"
#include "mafianet/socket_layer.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/assert.h"
#include "mafianet/rand.h"
#include "mafianet/message_identifiers.h"
#ifdef USE_THREADED_SEND
#include "mafianet/send_to_thread.h"
#endif
#include <math.h>
#include <algorithm>
#include <utility>
#include <vector>
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

using namespace MafiaNet;

namespace
{
// Re-split header flags (see ReliabilityLayer::AddReSplitHeader).
// A packet of the old copy was never sent: it cannot have been delivered.
const uint8_t RESPLIT_OLD_COPY_INCOMPLETE = 0x01;
// The old copy was a split message; its splitPacketId follows.
const uint8_t RESPLIT_HAS_OLD_SPLIT_ID = 0x02;
// The message after the header is itself a re-split copy, with a header of its own.
const uint8_t RESPLIT_INNER_IS_COPY = 0x04;
const uint8_t RESPLIT_KNOWN_FLAGS = RESPLIT_OLD_COPY_INCOMPLETE | RESPLIT_HAS_OLD_SPLIT_ID | RESPLIT_INNER_IS_COPY;
// Nesting: one level per step-down, and a connection steps down at most once per rung below the top.
const unsigned int RESPLIT_MAX_DEPTH = MTU_LADDER_SIZE - 1;
// One retired range on the wire: two u24 message numbers.
const unsigned int RESPLIT_RANGE_BYTES = 6;

// Whether reliable message number a was assigned before b (wrap-aware, as the receive window sees it).
bool IsOlderMessageNumber(MessageNumberType a, MessageNumberType b)
{
	const uint32_t distance = (b.val - a.val) & 0x00FFFFFFu;
	return distance != 0 && distance < 0x00800000u;
}
} // namespace

// #if defined(new)
// #pragma push_macro("new")
// #undef new
// #define RELIABILITY_LAYER_NEW_UNDEF_ALLOCATING_QUEUE
// #endif


//#define _DEBUG_LOGGER

#if CC_TIME_TYPE_BYTES == 4
static const CCTimeType MAX_TIME_BETWEEN_PACKETS = 350;  // 350 milliseconds
static const CCTimeType HISTOGRAM_RESTART_CYCLE = 10000; // Every 10 seconds reset the histogram
#else
static const CCTimeType MAX_TIME_BETWEEN_PACKETS = 350000; // 350 milliseconds
//static const CCTimeType HISTOGRAM_RESTART_CYCLE=10000000; // Every 10 seconds reset the histogram
#endif
static const CCTimeType STARTING_TIME_BETWEEN_PACKETS = MAX_TIME_BETWEEN_PACKETS;
//static const long double TIME_BETWEEN_PACKETS_INCREASE_MULTIPLIER_DEFAULT=.02;
//static const long double TIME_BETWEEN_PACKETS_DECREASE_MULTIPLIER_DEFAULT=1.0 / 9.0;

typedef uint32_t BitstreamLengthEncoding;

//#define PRINT_TO_FILE_RELIABLE_ORDERED_TEST
#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
static unsigned int packetNumber = 0;
static FILE *fp = 0;
#endif

//#define FLIP_SEND_ORDER_TEST
//#define LOG_TRIVIAL_NOTIFICATIONS

BPSTracker::TimeAndValue2::TimeAndValue2() {}
BPSTracker::TimeAndValue2::~TimeAndValue2() {}
BPSTracker::TimeAndValue2::TimeAndValue2(MafiaNet::TimeUS t, uint64_t v1)
    : value1(v1), time(t) {}
//BPSTracker::TimeAndValue2::TimeAndValue2(MafiaNet::TimeUS t, uint64_t v1, uint64_t v2) : time(t), value1(v1), value2(v2) {}
BPSTracker::BPSTracker()
{
	Reset();
}
BPSTracker::~BPSTracker() {}
void BPSTracker::Reset()
{
	total1 = lastSec1 = 0;
	dataQueue.clear();
}
//uint64_t BPSTracker::GetBPS2(RakNetTimeUS time) {ClearExpired2(time); return lastSec2;}
//void BPSTracker::GetBPS1And2(RakNetTimeUS time, uint64_t &out1, uint64_t &out2) {ClearExpired2(time); out1=lastSec1; out2=lastSec2;}
uint64_t BPSTracker::GetTotal1(void) const
{
	return total1;
}
//uint64_t BPSTracker::GetTotal2(void) const {return total2;}

void BPSTracker::ClearExpired1(MafiaNet::TimeUS time)
{
	while (!dataQueue.empty() &&
#if CC_TIME_TYPE_BYTES == 8
	       dataQueue.front().time + 1000000 < time
#else
	       dataQueue.front().time + 1000 < time
#endif
	)
	{
		lastSec1 -= dataQueue.front().value1;
		dataQueue.pop_front();
	}
}

struct DatagramHeaderFormat
{
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
	CCTimeType sourceSystemTime;
#endif
	DatagramSequenceNumberType datagramNumber;

	// Use floats to save bandwidth
	//	float B; // Link capacity
	float AS; // Data arrival rate
	bool isACK;
	bool isNAK;
	bool isPacketPair;
	bool hasBAndAS;
	bool isContinuousSend;
	bool needsBAndAs;
	bool isValid; // To differentiate between what I serialized, and offline data

	static BitSize_t GetDataHeaderBitLength()
	{
		return BYTES_TO_BITS(GetDataHeaderByteLength());
	}

	static unsigned int GetDataHeaderByteLength()
	{
		//return 2 + 3 + sizeof(MafiaNet::TimeMS) + sizeof(float)*2;
		return 2 + 3 +
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
		       sizeof(RakNetTimeMS) +
#endif
		       sizeof(float) * 1;
	}

	void Serialize(MafiaNet::BitStream *b)
	{
		// Not endian safe
		//		RakAssert(GetDataHeaderByteLength()==sizeof(DatagramHeaderFormat));
		//		b->WriteAlignedBytes((const unsigned char*) this, sizeof(DatagramHeaderFormat));
		//		return;

		b->Write(true); // IsValid
		if (isACK)
		{
			b->Write(true); // IsACK
			b->Write(hasBAndAS);
			b->AlignWriteToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
			MafiaNet::TimeMS timeMSLow = (MafiaNet::TimeMS) sourceSystemTime & 0xFFFFFFFF;
			b->Write(timeMSLow);
#endif
			if (hasBAndAS)
			{
				//		b->Write(B);
				b->Write(AS);
			}
		}
		else if (isNAK)
		{
			b->Write(false); // isACK
			b->Write(true);  // isNAK
		}
		else
		{
			b->Write(false); // isACK
			b->Write(false); // isNAK
			b->Write(isPacketPair);
			b->Write(isContinuousSend);
			b->Write(needsBAndAs);
			b->AlignWriteToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
			MafiaNet::TimeMS timeMSLow = (MafiaNet::TimeMS) sourceSystemTime & 0xFFFFFFFF;
			b->Write(timeMSLow);
#endif
			b->Write(datagramNumber);
		}
	}

	void Deserialize(MafiaNet::BitStream *b)
	{
		// Not endian safe
		//		b->ReadAlignedBytes((unsigned char*) this, sizeof(DatagramHeaderFormat));
		//		return;

		b->Read(isValid);
		b->Read(isACK);
		if (isACK)
		{
			isNAK = false;
			isPacketPair = false;
			b->Read(hasBAndAS);
			b->AlignReadToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
			MafiaNet::TimeMS timeMS;
			b->Read(timeMS);
			sourceSystemTime = (CCTimeType) timeMS;
#endif
			if (hasBAndAS)
			{
				//			b->Read(B);
				b->Read(AS);
			}
		}
		else
		{
			b->Read(isNAK);
			if (isNAK)
			{
				isPacketPair = false;
			}
			else
			{
				b->Read(isPacketPair);
				b->Read(isContinuousSend);
				b->Read(needsBAndAs);
				b->AlignReadToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
				MafiaNet::TimeMS timeMS;
				b->Read(timeMS);
				sourceSystemTime = (CCTimeType) timeMS;
#endif
				b->Read(datagramNumber);
			}
		}
	}
};

#ifdef _WIN32
//#define _DEBUG_LOGGER
#ifdef _DEBUG_LOGGER
#include "mafianet/windows_includes.h"
#endif
#endif

//#define DEBUG_SPLIT_PACKET_PROBLEMS
#if defined(DEBUG_SPLIT_PACKET_PROBLEMS)
static int waitFlag = -1;
#endif

using namespace MafiaNet;

SplitPacketSort::SplitPacketSort()
    : m_count(0),
      m_packetId(0)
{
}

void SplitPacketSort::Init(const InternalPacket *firstFragment)
{
	RakAssert(m_fragments.empty());
	m_count = firstFragment->splitPacketCount;
	m_packetId = firstFragment->splitPacketId;
}

bool SplitPacketSort::AllPacketsAdded() const
{
	return m_count > 0 && m_fragments.size() == m_count;
}

unsigned int SplitPacketSort::GetNumAddedPackets() const
{
	return (unsigned int) m_fragments.size();
}

SplitPacketIdType SplitPacketSort::GetPacketId() const
{
	return m_packetId;
}

const std::map<SplitPacketIndexType, InternalPacket *> &SplitPacketSort::Fragments() const
{
	return m_fragments;
}

bool SplitPacketSort::Add(InternalPacket *internalPacket)
{
	const SplitPacketIndexType index = internalPacket->splitPacketIndex;
	if (internalPacket->splitPacketId != m_packetId ||
	    internalPacket->splitPacketCount != m_count ||
	    index >= m_count)
		return false;
	// An honest sender splits on byte boundaries; only the last fragment may
	// end mid-byte. Anything else would overrun the reassembly buffer.
	if (index + 1 < m_count && (internalPacket->dataBitLength & 7) != 0)
		return false;
	return m_fragments.emplace(index, internalPacket).second;
}


//-------------------------------------------------------------------------------------------------------
// Constructor
//-------------------------------------------------------------------------------------------------------
ReliabilityLayer::ReliabilityLayer()
    : maxReceiveWindow(1000000), reSplitNumbersMarked(0), encryptionMode(EncryptionMode::Required), cipherOverheadBytes(0), droppedUnauthenticatedDatagrams(0), droppedKeylessSends(0)
{

#ifdef _DEBUG
	// Wait longer to disconnect in debug so I don't get disconnected while tracing
	timeoutTime = 30000;
#else
	timeoutTime = 10000;
#endif

#ifdef _DEBUG
	minExtraPing = extraPingVariance = 0;
	packetloss = (double) minExtraPing;
#endif


#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
	if (fp == 0 && 0)
	{
		fopen_s(&fp, "reliableorderedoutput.txt", "wt");
	}
#endif

	InitializeVariables();
}

//-------------------------------------------------------------------------------------------------------
// Destructor
//-------------------------------------------------------------------------------------------------------
ReliabilityLayer::~ReliabilityLayer()
{
	FreeMemory(true); // Free all memory immediately
}
//-------------------------------------------------------------------------------------------------------
// Resets the layer for reuse
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::Reset(bool resetVariables, int mtuSize)
{
	FreeMemory(true); // true because making a memory reset pending in the update cycle causes resets after reconnects.  Instead, just call Reset from a single thread
	if (resetVariables)
	{
		InitializeVariables();
		// The cipher overhead is budgeted here, once: every datagram size the layer
		// computes derives from the congestion manager's MTU. currentMtuBytes stays
		// the wire MTU, which sealed datagrams then fill exactly.
		cipherOverheadBytes = encryptionMode == EncryptionMode::Required ? DatagramCipher::kMaxOverhead : 0;
		congestionManager.Init(MafiaNet::GetTimeUS(), mtuSize - UDP_HEADER_SIZE - cipherOverheadBytes);
	}
	currentMtuBytes = mtuSize;
}

//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SetEncryptionMode(EncryptionMode mode)
{
	encryptionMode = mode;
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::SetSessionKeys(const SessionKeys &keys)
{
	// Sealed datagrams only fit when Reset() reserved the overhead (mode Required).
	if (encryptionMode != EncryptionMode::Required || cipherOverheadBytes != DatagramCipher::kMaxOverhead)
		return false;
	datagramCipher.SetKeys(keys);
	return datagramCipher.HasKeys();
}

//-------------------------------------------------------------------------------------------------------
uint64_t ReliabilityLayer::GetDroppedKeylessSendsForTesting() const
{
	return droppedKeylessSends;
}

//-------------------------------------------------------------------------------------------------------
unsigned int ReliabilityLayer::GetReceiveWindowSizeForTesting() const
{
	return (unsigned int) hasReceivedPacketQueue.size();
}

//-------------------------------------------------------------------------------------------------------
unsigned int ReliabilityLayer::GetPartialSplitMessagesForTesting() const
{
	return (unsigned int) splitPacketChannelList.size();
}

//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SetMaxReceiveWindowForTesting(uint32_t messages)
{
	maxReceiveWindow = messages;
}

//-------------------------------------------------------------------------------------------------------
uint64_t ReliabilityLayer::GetReSplitNumbersMarkedForTesting() const
{
	return reSplitNumbersMarked;
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsEncrypted() const
{
	return datagramCipher.HasKeys();
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::GetSessionKeysForTesting(SessionKeys *out) const
{
	return datagramCipher.GetKeysForTesting(out);
}

//-------------------------------------------------------------------------------------------------------
int ReliabilityLayer::GetCurrentMtuBytes(void) const
{
	return currentMtuBytes;
}

//-------------------------------------------------------------------------------------------------------
// Set the time, in MS, to use before considering ourselves disconnected after not being able to deliver a reliable packet
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SetTimeoutTime(MafiaNet::TimeMS time)
{
	timeoutTime = time;
}

//-------------------------------------------------------------------------------------------------------
// Returns the value passed to SetTimeoutTime. or the default if it was never called
//-------------------------------------------------------------------------------------------------------
MafiaNet::TimeMS ReliabilityLayer::GetTimeoutTime()
{
	return timeoutTime;
}

//-------------------------------------------------------------------------------------------------------
// Initialize the variables
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::InitializeVariables()
{
	droppedUnauthenticatedDatagrams = 0;
	droppedKeylessSends = 0;
	// OrderingIndexType (uint24_t) is a wrapper around one uint32_t; assigning 0 leaves the
	// same all-zero state the memset used to, without memset-ing a non-trivial class.
	for (unsigned int stream = 0; stream < NUMBER_OF_ORDERED_STREAMS; stream++)
	{
		orderedWriteIndex[stream] = 0;
		sequencedWriteIndex[stream] = 0;
		orderedReadIndex[stream] = 0;
		highestSequencedReadIndex[stream] = 0;
	}
	memset(&statistics, 0, sizeof(statistics));
	memset(&heapIndexOffsets, 0, sizeof(heapIndexOffsets));

	statistics.connectionStartTime = MafiaNet::GetTimeUS();
	currentMtuBytes = MAXIMUM_MTU_SIZE;
	mtuBlackHoleEvidence.Reset();
	mtuStepDowns = 0;
	splitPacketId = 0;
	outgoingCreditBitMicros = 0;
	throughputCapCountdown = 0;
	sendReliableMessageNumberIndex = 0;
	internalOrderIndex = 0;
	timeToNextUnreliableCull = 0;
	unreliableLinkedListHead = 0;
	lastUpdateTime = MafiaNet::GetTimeUS();
	bandwidthExceededStatistic = false;
	remoteSystemTime = 0;
	unreliableTimeout = 0;
	lastBpsClear = 0;

	// Disable packet pairs
	countdownToNextPacketPair = 15;

	nextAllowedThroughputSample = 0;
	deadConnection = cheater = false;
	timeOfLastContinualSend = 0;

	// timeResendQueueNonEmpty = 0;
	timeLastDatagramArrived = MafiaNet::GetTimeMS();
	//	packetlossThisSample=false;
	//	backoffThisSample=0;
	//	packetlossThisSampleResendCount=0;
	//	lastPacketlossTime=0;
	statistics.messagesInResendBuffer = 0;
	statistics.bytesInResendBuffer = 0;

	receivedPacketsBaseIndex = 0;
	resetReceivedPackets = true;
	receivePacketCount = 0;

	//	SetPing( 1000 );

	timeBetweenPackets = STARTING_TIME_BETWEEN_PACKETS;

	ackPingIndex = 0;
	ackPingSum = (CCTimeType) 0;

	nextSendTime = lastUpdateTime;
	//nextLowestPingReset=(CCTimeType)0;
	//	continuousSend=false;

	//	histogramStart=(CCTimeType)0;
	//	histogramBitsSent=0;
	unacknowledgedBytes = 0;
	resendLinkedListHead = 0;
	totalUserDataBytesAcked = 0;

	datagramHistoryPopCount = 0;

	InitHeapWeights();
	for (unsigned int i = 0; i < MafiaNet::NUMBER_OF_PRIORITIES; i++)
	{
		statistics.messageInSendBuffer[i] = 0;
		statistics.bytesInSendBuffer[i] = 0.0;
	}

	for (int i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++)
	{
		bpsMetrics[i].Reset();
	}
}

//-------------------------------------------------------------------------------------------------------
// Frees all allocated memory
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::FreeMemory(bool freeAllImmediately)
{
	(void) freeAllImmediately;
	FreeThreadSafeMemory();
	// The session ends with the memory: wipe its keys, sequence and replay window.
	datagramCipher.Clear();
}

void ReliabilityLayer::FreeThreadSafeMemory(void)
{
	unsigned i;
	InternalPacket *internalPacket;

	ClearPacketsAndDatagrams();

	for (const auto &channelEntry : splitPacketChannelList)
	{
		SplitPacketChannel *channel = channelEntry.second;
		for (const auto &fragment : channel->splitPacketList.Fragments())
		{
			internalPacket = fragment.second;
			FreeInternalPacketData(internalPacket);
			ReleaseToInternalPacketPool(internalPacket);
		}
#if PREALLOCATE_LARGE_MESSAGES == 1
		if (channel->returnedPacket)
		{
			FreeInternalPacketData(channel->returnedPacket);
			ReleaseToInternalPacketPool(channel->returnedPacket);
		}
#endif
		delete channel;
	}
	splitPacketChannelList.clear();

	for (InternalPacket *queued : outputQueue)
	{
		FreeInternalPacketData(queued);
		ReleaseToInternalPacketPool(queued);
	}
	outputQueue.clear();


	for (i = 0; i < NUMBER_OF_ORDERED_STREAMS; i++)
	{
		for (const HeapEntry &entry : orderingHeaps[i])
		{
			FreeInternalPacketData(entry.packet);
			ReleaseToInternalPacketPool(entry.packet);
		}
		orderingHeaps[i].clear();
	}

	//resendList.ForEachData(DeleteInternalPacket);
	//	resendTree.clear();
	memset(resendBuffer, 0, sizeof(resendBuffer));
	statistics.messagesInResendBuffer = 0;
	statistics.bytesInResendBuffer = 0;

	if (resendLinkedListHead)
	{
		InternalPacket *prev;
		InternalPacket *iter = resendLinkedListHead;

		for (;;)
		{
			if (iter->data)
				FreeInternalPacketData(iter);
			prev = iter;
			iter = iter->resendNext;
			if (iter == resendLinkedListHead)
			{
				ReleaseToInternalPacketPool(prev);
				break;
			}
			ReleaseToInternalPacketPool(prev);
		}
		resendLinkedListHead = 0;
	}
	unacknowledgedBytes = 0;

	//	acknowlegements.clear();

	for (const HeapEntry &entry : outgoingPacketBuffer)
	{
		if (entry.packet->data)
			FreeInternalPacketData(entry.packet);
		ReleaseToInternalPacketPool(entry.packet);
	}

	outgoingPacketBuffer.clear();

#ifdef _DEBUG
	for (DataAndTime *delayed : delayList)
		delete delayed;
	delayList.clear();
#endif

	unreliableWithAckReceiptHistory.clear();

	packetsToSendThisUpdate.clear();
	packetsToSendThisUpdate.reserve(512);
	packetsToDeallocThisUpdate.clear();
	packetsToDeallocThisUpdate.reserve(512);
	packetsToSendThisUpdateDatagramBoundaries.clear();
	packetsToSendThisUpdateDatagramBoundaries.reserve(128);
	datagramSizesInBytes.clear();
	datagramSizesInBytes.reserve(128);

	// Everything acquired was released above; drop the spares too.
	internalPacketPool.Clear();

	while (!datagramHistory.empty())
	{
		RemoveFromDatagramHistory(datagramHistoryPopCount);
		datagramHistory.pop_front();
		datagramHistoryPopCount++;
	}
	datagramHistoryPopCount = 0;

	acknowlegements.Clear();
	NAKs.Clear();
	datagramGaps.Clear();

	unreliableLinkedListHead = 0;
}

//-------------------------------------------------------------------------------------------------------
// Packets are read directly from the socket layer and skip the reliability
//layer  because unconnected players do not use the reliability layer
// This function takes packet data after a player has been confirmed as
//connected.  The game should not use that data directly
// because some data is used internally, such as packet acknowledgment and
//split packets
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::HandleSocketReceiveFromConnectedPlayer(
    const char *buffer, unsigned int length, SystemAddress &systemAddress, std::vector<PluginInterface2 *> &messageHandlerList, int mtuSize,
    RakNetSocket2 *s, RakNetRandom *rnr, CCTimeType timeRead, BitStream &updateBitStream)
{
	// unreferenced parameters
	(void) mtuSize;

	RakAssert(buffer != nullptr);

#if CC_TIME_TYPE_BYTES == 4
	timeRead /= 1000;
#endif

	bpsMetrics[(int) ACTUAL_BYTES_RECEIVED].Push1(timeRead, length);

	// Mode Required without keys: nothing is accepted. There is no plaintext
	// fallback, even if a caller installed the layer without its session keys.
	if (encryptionMode == EncryptionMode::Required && !datagramCipher.HasKeys())
	{
		droppedUnauthenticatedDatagrams++;
		return true;
	}

	// Once the session is keyed, nothing is parsed (or even counted as a sign of
	// life) before it authenticates. Open into a private copy: the caller's buffer
	// stays untouched, and a failed Open has wiped the copy, which is discarded.
	unsigned char opened[MAXIMUM_MTU_SIZE];
	if (datagramCipher.HasKeys() && buffer != nullptr)
	{
		uint8_t *plaintext = nullptr;
		int plaintextLength = -1;
		if (length <= sizeof(opened))
		{
			memcpy(opened, buffer, length);
			plaintextLength = datagramCipher.Open(opened, (int) length, &plaintext);
		}
		if (plaintextLength < 0)
		{
			// Forged, corrupted, replayed or too old: drop and count. No plugin
			// notification, so a flood of junk cannot become a flood of callbacks.
			droppedUnauthenticatedDatagrams++;
			return true;
		}
		buffer = (const char *) plaintext;
		length = (unsigned int) plaintextLength;
	}

	if (length <= 2 || buffer == nullptr)
	{ // Length of 1 is a connection request resend that we just ignore
		for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
			messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("length <= 2 || buffer == nullptr", BYTES_TO_BITS(length), systemAddress, true);
		return true;
	}

	timeLastDatagramArrived = MafiaNet::GetTimeMS();

	//	CCTimeType time;
	//	bool indexFound;
	//	int count, size;
	DatagramSequenceNumberType holeCount;
	unsigned i;

	MafiaNet::BitStream socketData((unsigned char *) buffer, length, false); // Convert the incoming data to a bitstream for easy parsing
	//	time = MafiaNet::GetTimeUS();

	// Set to the current time if it is not zero, and we get incoming data
	// 	if (timeResendQueueNonEmpty!=0)
	// 		timeResendQueueNonEmpty=timeRead;

	DatagramHeaderFormat dhf;
	dhf.Deserialize(&socketData);
	if (!dhf.isValid)
	{
		for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
		{
			messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("!dhf.isValid", BYTES_TO_BITS(length), systemAddress, true);
		}

		return true;
	}
	if (dhf.isACK)
	{
		DatagramSequenceNumberType datagramNumber;
		// datagramNumber=dhf.datagramNumber;

#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
		MafiaNet::TimeMS timeMSLow = (MafiaNet::TimeMS) timeRead & 0xFFFFFFFF;
		CCTimeType rtt = timeMSLow - dhf.sourceSystemTime;
#if CC_TIME_TYPE_BYTES == 4
		if (rtt > 10000)
#else
		if (rtt > 10000000)
#endif
		{
			// Sanity check. This could happen due to type overflow, especially since I only send the low 4 bytes to reduce bandwidth
			rtt = (CCTimeType) congestionManager.GetRTT();
		}
		//	RakAssert(rtt < 500000);
		//	printf("%i ", (MafiaNet::TimeMS)(rtt/1000));
		ackPing = rtt;
#endif

#ifdef _DEBUG
		if (!dhf.hasBAndAS)
		{
			//			dhf.B=0;
			dhf.AS = 0;
		}
#endif
		//		congestionManager.OnAck(timeRead, rtt, dhf.hasBAndAS, dhf.B, dhf.AS, totalUserDataBytesAcked );

		incomingAcks.Clear();
		if (!incomingAcks.Deserialize(&socketData))
		{
			for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
			{
				messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("incomingAcks.Deserialize failed", BYTES_TO_BITS(length), systemAddress, true);
			}

			return false;
		}

		std::size_t k = 0;
		while (k < unreliableWithAckReceiptHistory.size())
		{
			if (incomingAcks.IsWithinRange(unreliableWithAckReceiptHistory[k].datagramNumber))
			{
				InternalPacket *ackReceipt = AllocateFromInternalPacketPool();
				AllocInternalPacketData(ackReceipt, 5, false);
				ackReceipt->dataBitLength = BYTES_TO_BITS(5);
				ackReceipt->data[0] = (MessageID) ID_SND_RECEIPT_ACKED;
				memcpy(ackReceipt->data + sizeof(MessageID), &unreliableWithAckReceiptHistory[k].sendReceiptSerial, sizeof(uint32_t));
				outputQueue.push_back(ackReceipt);

				// Remove, keeping the order of the rest
				unreliableWithAckReceiptHistory.erase(unreliableWithAckReceiptHistory.begin() + k);
			}
			else
			{
				k++;
			}
		}

		// early out, if we've got no outstanding datagramHistory entries
		if (datagramHistory.empty())
		{
			receivePacketCount++;
			return true;
		}

		for (i = 0; i < incomingAcks.ranges.size(); i++)
		{
			// note: minIndex is ensured to be always <= maxIndex - otherwise Deserialize() would have failed
			RakAssert(incomingAcks.ranges[i].minIndex <= incomingAcks.ranges[i].maxIndex);

			if (incomingAcks.ranges[i].maxIndex == (uint24_t) (0xFFFFFFFF))
			{
				for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
				{
					messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("incomingAcks maxIndex is max value", BYTES_TO_BITS(length), systemAddress, true);
				}

				// it's an invalid incoming package --- let's abort processing (there's no point in continuing processing other ranges if the package is invalid)
				return false;
			}

			for (datagramNumber = incomingAcks.ranges[i].minIndex; datagramNumber <= incomingAcks.ranges[i].maxIndex; datagramNumber++)
			{
				const DatagramSequenceNumberType offsetIntoList = datagramNumber - datagramHistoryPopCount;
				if (offsetIntoList >= datagramHistory.size())
				{
					// reached the end of the datagramHistory list - hence, we are done
					receivePacketCount++;
					return true;
				}

				CCTimeType whenSent;
				int datagramWireBytes = 0;
				MessageNumberNode *messageNumberNode = GetMessageNumberNodeByDatagramIndex(datagramNumber, &whenSent, &datagramWireBytes);
				// This size got through: failures of datagrams no larger were loss, not a black hole.
				mtuBlackHoleEvidence.OnDatagramAcked(datagramWireBytes);
				if (messageNumberNode)
				{
					//	printf("%p Got ack for %i\n", this, datagramNumber.val);
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
					congestionManager.OnAck(timeRead, rtt, dhf.hasBAndAS, 0, dhf.AS, totalUserDataBytesAcked, bandwidthExceededStatistic, datagramNumber);
#else
					CCTimeType ping;
					if (timeRead > whenSent)
					{
						ping = timeRead - whenSent;
					}
					else
					{
						ping = 0;
					}
					congestionManager.OnAck(timeRead, ping, dhf.hasBAndAS, 0, dhf.AS, totalUserDataBytesAcked, bandwidthExceededStatistic, datagramNumber);
#endif
					while (messageNumberNode)
					{
						// TESTING1
						// 						printf("Remove %i on ack for datagramNumber=%i.\n", messageNumberNode->messageNumber.val, datagramNumber.val);

						RemovePacketFromResendListAndDeleteOlderReliableSequenced(messageNumberNode->messageNumber, timeRead, messageHandlerList, systemAddress);
						messageNumberNode = messageNumberNode->next;
					}

					RemoveFromDatagramHistory(datagramNumber);
				}
				// 				else if (isReliable) {
				// 					// Previously used slot, rather than empty unreliable slot
				// 					printf("%p Ack %i is duplicate\n", this, datagramNumber.val);
				//
				//  				congestionManager.OnDuplicateAck(timeRead, datagramNumber);
				// 				}
			}
		}
	}
	else if (dhf.isNAK)
	{
		// early out, if we've got no outstanding datagramHistory entries
		if (datagramHistory.empty())
		{
			receivePacketCount++;
			return true;
		}

		DatagramSequenceNumberType messageNumber;
		RangeList<DatagramSequenceNumberType> incomingNAKs;
		if (!incomingNAKs.Deserialize(&socketData))
		{
			for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
			{
				messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("incomingNAKs.Deserialize failed", BYTES_TO_BITS(length), systemAddress, true);
			}

			// it's an invalid incoming package --- let's abort processing (there's no point in continuing processing other ranges if the package is invalid)
			return false;
		}
		for (i = 0; i < incomingNAKs.ranges.size(); i++)
		{
			// note: minIndex is ensured to be always <= maxIndex - otherwise Deserialize() would have failed
			RakAssert(incomingNAKs.ranges[i].minIndex <= incomingNAKs.ranges[i].maxIndex);

			if (incomingNAKs.ranges[i].maxIndex == (uint24_t) (0xFFFFFFFF))
			{
				for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
				{
					messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("incomingNAKs maxIndex is max value", BYTES_TO_BITS(length), systemAddress, true);
				}

				return false;
			}
			// Sanity check
			//RakAssert(incomingNAKs.ranges[i].maxIndex.val-incomingNAKs.ranges[i].minIndex.val<1000);
			for (messageNumber = incomingNAKs.ranges[i].minIndex; messageNumber <= incomingNAKs.ranges[i].maxIndex; messageNumber++)
			{
				congestionManager.OnNAK(timeRead, messageNumber);

				// REMOVEME
				//				printf("%p NAK %i\n", this, dhf.datagramNumber.val);

				const DatagramSequenceNumberType offsetIntoList = messageNumber - datagramHistoryPopCount;
				if (offsetIntoList >= datagramHistory.size())
				{
					// reached the end of the datagramHistory list - hence, we are done
					receivePacketCount++;
					return true;
				}

				CCTimeType timeSent;
				MessageNumberNode *messageNumberNode = GetMessageNumberNodeByDatagramIndex(messageNumber, &timeSent);
				while (messageNumberNode)
				{
					// Update timers so resends occur immediately
					InternalPacket *internalPacket = resendBuffer[messageNumberNode->messageNumber & (uint32_t) RESEND_BUFFER_ARRAY_MASK];
					if (internalPacket)
					{
						if (internalPacket->nextActionTime != 0)
						{
							internalPacket->nextActionTime = timeRead;
						}
					}

					messageNumberNode = messageNumberNode->next;
				}
			}
		}
	}
	else
	{
		uint32_t skippedMessageCount;
		if (!congestionManager.OnGotPacket(dhf.datagramNumber, dhf.isContinuousSend, timeRead, length, &skippedMessageCount))
		{
			for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
			{
				messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("congestionManager.OnGotPacket failed", BYTES_TO_BITS(length), systemAddress, true);
			}

			return true;
		}
		if (dhf.isPacketPair)
		{
			congestionManager.OnGotPacketPair(dhf.datagramNumber, length, timeRead);
		}

		constexpr unsigned microsPerUnit = CC_TIME_TYPE_BYTES == 4 ? 1000 : 1;
		datagramGaps.Receive(dhf.datagramNumber, skippedMessageCount, uint64_t{timeRead} * microsPerUnit,
		                     static_cast<TimeUS>(congestionManager.GetRTT() * microsPerUnit), NAKs);
		remoteSystemNeedsBAndAS = dhf.needsBAndAs;

		// Ack dhf.datagramNumber
		// Ack even unreliable messages for congestion control, just don't resend them on no ack
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
		SendAcknowledgementPacket(dhf.datagramNumber, dhf.sourceSystemTime);
#else
		SendAcknowledgementPacket(dhf.datagramNumber, 0);
#endif

		InternalPacket *internalPacket = CreateInternalPacketFromBitStream(&socketData, timeRead);
		if (internalPacket == 0)
		{
			for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
			{
				messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("CreateInternalPacketFromBitStream failed", BYTES_TO_BITS(length), systemAddress, true);
			}

			return true;
		}

		while (internalPacket)
		{
			for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
			{
#if CC_TIME_TYPE_BYTES == 4
				messageHandlerList[messageHandlerIndex]->OnInternalPacket(internalPacket, receivePacketCount, systemAddress, timeRead, false);
#else
				messageHandlerList[messageHandlerIndex]->OnInternalPacket(internalPacket, receivePacketCount, systemAddress, (MafiaNet::TimeMS)(timeRead / (CCTimeType) 1000), false);
#endif
			}

			{

				// resetReceivedPackets is set from a non-threadsafe function.
				// We do the actual reset in this function so the data is not modified by multiple threads
				if (resetReceivedPackets)
				{
					hasReceivedPacketQueue.clear();
					receivedPacketsBaseIndex = 0;
					resetReceivedPackets = false;
				}

				// Check for corrupt orderingChannel
				if (
				    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
				    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
				    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered)
				{
					if (internalPacket->orderingChannel >= NUMBER_OF_ORDERED_STREAMS)
					{
						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
						{
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("internalPacket->orderingChannel >= NUMBER_OF_ORDERED_STREAMS", BYTES_TO_BITS(length), systemAddress, true);
						}

						bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

						FreeInternalPacketData(internalPacket);
						ReleaseToInternalPacketPool(internalPacket);
						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
				}

				// 8/12/09 was previously not checking if the message was reliable. However, on packetloss this would mean you'd eventually exceed the
				// hole count because unreliable messages were never resent, and you'd stop getting messages
				if (internalPacket->reliability == MafiaNet::Reliability::Reliable || internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced || internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered)
				{
					// If the following conditional is true then this either a duplicate packet
					// or an older out of order packet
					// The subtraction unsigned overflow is intentional
					holeCount = (DatagramSequenceNumberType) (internalPacket->reliableMessageNumber - receivedPacketsBaseIndex);
					const DatagramSequenceNumberType typeRange = (DatagramSequenceNumberType) (uint32_t) -1;

					// TESTING1
					// 					printf("waiting on reliableMessageNumber=%i holeCount=%i datagramNumber=%i\n", receivedPacketsBaseIndex.val, holeCount.val, dhf.datagramNumber.val);

					if (holeCount == (DatagramSequenceNumberType) 0)
					{
						// Got what we were expecting
						if (!hasReceivedPacketQueue.empty())
						{
							hasReceivedPacketQueue.pop_front();
						}
						++receivedPacketsBaseIndex;
					}
					else if (holeCount > typeRange / (DatagramSequenceNumberType) 2)
					{
						bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
						{
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("holeCount > typeRange/(DatagramSequenceNumberType) 2", BYTES_TO_BITS(length), systemAddress, false);
						}

						// Duplicate packet
						FreeInternalPacketData(internalPacket);
						ReleaseToInternalPacketPool(internalPacket);

						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
					else if ((unsigned int) holeCount < hasReceivedPacketQueue.size())
					{
						// Got a higher count out of order packet that was missing in the sequence or we already got
						if (hasReceivedPacketQueue[(uint32_t) holeCount] != false)
						{ // non-zero means this is a hole
#ifdef LOG_TRIVIAL_NOTIFICATIONS
							for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
							{
								messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Higher count pushed to hasReceivedPacketQueue", BYTES_TO_BITS(length), systemAddress, false);
							}
#endif

							// Fill in the hole
							hasReceivedPacketQueue[(uint32_t) holeCount] = false; // We got the packet at holeCount
						}
						else
						{
							bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

#ifdef LOG_TRIVIAL_NOTIFICATIONS
							for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
							{
								messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Duplicate packet ignored", BYTES_TO_BITS(length), systemAddress, false);
							}
#endif

							// Duplicate packet
							FreeInternalPacketData(internalPacket);
							ReleaseToInternalPacketPool(internalPacket);

							goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
						}
					}
					else
					{ // holeCount>=receivedPackets.Size()
						if (holeCount > (DatagramSequenceNumberType) maxReceiveWindow)
						{
							// A peer jumped its message numbering far ahead: drop the
							// packet. No RakAssert -- the remote chooses this value.
							for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
							{
								messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("holeCount > 1000000", BYTES_TO_BITS(length), systemAddress, true);
							}

							bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

							// Would crash due to out of memory!
							FreeInternalPacketData(internalPacket);
							ReleaseToInternalPacketPool(internalPacket);

							goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
						}

#ifdef LOG_TRIVIAL_NOTIFICATIONS
						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
						{
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Adding to hasReceivedPacketQueue later ordered message", BYTES_TO_BITS(length), systemAddress, false);
						}
#endif

						// Fix - sending on a higher priority gives us a very very high received packets base index if we formerly had pre-split a lot of messages and
						// used that as the message number.  Because of this, a lot of time is spent in this linear loop and the timeout time expires because not
						// all of the message is sent in time.
						// Fixed by late assigning message IDs on the sender

						// Add 0 times to the queue until (reliableMessageNumber - baseIndex) < queue size.
						while ((unsigned int) (holeCount) > hasReceivedPacketQueue.size())
						{
							hasReceivedPacketQueue.push_back(true); // Didn't get this packet: a hole
						}
						hasReceivedPacketQueue.push_back(false); // Got the packet
#ifdef _DEBUG
						// If this assert hits then DatagramSequenceNumberType has overflowed
						RakAssert(hasReceivedPacketQueue.size() < (unsigned int) ((DatagramSequenceNumberType) (uint32_t) (-1)));
#endif
					}

					while (!hasReceivedPacketQueue.empty() && !hasReceivedPacketQueue.front())
					{
						hasReceivedPacketQueue.pop_front();
						++receivedPacketsBaseIndex;
					}
				}


				/*
				if (internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced || internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced) {
#ifdef _DEBUG
					RakAssert(internalPacket->orderingChannel < NUMBER_OF_ORDERED_STREAMS);
#endif

					if (internalPacket->orderingChannel >= NUMBER_OF_ORDERED_STREAMS) {
						FreeInternalPacketData(internalPacket);
						ReleaseToInternalPacketPool(internalPacket);

						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++) {
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("internalPacket->orderingChannel >= NUMBER_OF_ORDERED_STREAMS", BYTES_TO_BITS(length), systemAddress);
						}

						bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead,BITS_TO_BYTES(internalPacket->dataBitLength));

						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}

					if (!IsOlderOrderedPacket(internalPacket->orderingIndex, waitingForSequencedPacketReadIndex[internalPacket->orderingChannel])) {
						// Is this a split packet?
						if (internalPacket->splitPacketCount > 0) {
							// Generate the split
							// Verify some parameters to make sure we don't get junk data


							// Check for a rebuilt packet
							InsertIntoSplitPacketList(internalPacket, timeRead);
							bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(timeRead,BITS_TO_BYTES(internalPacket->dataBitLength));

							// Sequenced
							internalPacket = BuildPacketFromSplitPacketList( internalPacket->splitPacketId, timeRead,
								s, systemAddress, rnr, extraSocketOptions);

							if (internalPacket) {
								// Update our index to the newest packet
								waitingForSequencedPacketReadIndex[ internalPacket->orderingChannel ] = internalPacket->orderingIndex + (OrderingIndexType)1;

								// If there is a rebuilt packet, add it to the output queue
								outputQueue.push_back(internalPacket);
								internalPacket = 0;
							}

							// else don't have all the parts yet
						} else {
							// Update our index to the newest packet
							waitingForSequencedPacketReadIndex[ internalPacket->orderingChannel ] = internalPacket->orderingIndex + (OrderingIndexType)1;

							// Not a split packet. Add the packet to the output queue
							bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(timeRead,BITS_TO_BYTES(internalPacket->dataBitLength));
							outputQueue.push_back(internalPacket);
							internalPacket = 0;
						}
					} else {
						// Older sequenced packet. Discard it
						FreeInternalPacketData(internalPacket);
						ReleaseToInternalPacketPool( internalPacket );

						bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead,BITS_TO_BYTES(internalPacket->dataBitLength));
					}

					goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
				}

				// Is this an unsequenced split packet?
				if ( internalPacket->splitPacketCount > 0 )
				{
					// Check for a rebuilt packet
					if ( internalPacket->reliability != MafiaNet::Reliability::ReliableOrdered )
						internalPacket->orderingChannel = 255; // Use 255 to designate not sequenced and not ordered

					// Insert may free the fragment (duplicate or malformed): read its id first.
					const SplitPacketIdType fragmentMessageId = internalPacket->splitPacketId;
					InsertIntoSplitPacketList( internalPacket, timeRead );

					internalPacket = BuildPacketFromSplitPacketList( fragmentMessageId, timeRead,
						s, systemAddress, rnr, extraSocketOptions);

					if ( internalPacket == 0 )
					{

						// Don't have all the parts yet
						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
				}
				*/


				// Is this a split packet? If so then reassemble
				if (internalPacket->splitPacketCount > 0)
				{
					// Check for a rebuilt packet
					if (internalPacket->reliability != MafiaNet::Reliability::ReliableOrdered && internalPacket->reliability != MafiaNet::Reliability::ReliableSequenced && internalPacket->reliability != MafiaNet::Reliability::UnreliableSequenced)
						internalPacket->orderingChannel = 255; // Use 255 to designate not sequenced and not ordered

					// Insert may free the fragment (duplicate or malformed): read its id first.
					const SplitPacketIdType fragmentMessageId = internalPacket->splitPacketId;
					InsertIntoSplitPacketList(internalPacket, timeRead);

					internalPacket = BuildPacketFromSplitPacketList(fragmentMessageId, timeRead,
					                                                s, systemAddress, rnr, updateBitStream);

					if (internalPacket == 0)
					{
#ifdef LOG_TRIVIAL_NOTIFICATIONS
						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("BuildPacketFromSplitPacketList did not return anything.", BYTES_TO_BITS(length), systemAddress, false);
#endif

						// Don't have all the parts yet
						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
				}

				// A copy the sender re-split after an MTU step-down: close the message numbers it
				// retired, and deliver it only if the copy it replaces was not delivered.
				if (internalPacket->resplitCopy)
				{
					const BitSize_t copyBitLength = internalPacket->dataBitLength;
					const ReSplitVerdict verdict = ResolveReSplitCopy(internalPacket);
					if (verdict != RESPLIT_DELIVER)
					{
						if (verdict == RESPLIT_MALFORMED)
						{
							for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
								messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Malformed re-split header", BYTES_TO_BITS(length), systemAddress, true);
						}
						bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead, BITS_TO_BYTES(copyBitLength));
						FreeInternalPacketData(internalPacket);
						ReleaseToInternalPacketPool(internalPacket);
						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
				}

#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
				unsigned char packetId;
				char *type = "UNDEFINED";
#endif
				if (internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
				    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
				    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered)
				{
#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST

					// ___________________
					BitStream bitStream(internalPacket->data, BITS_TO_BYTES(internalPacket->dataBitLength), false);
					unsigned int receivedPacketNumber;
					MafiaNet::Time receivedTime;
					unsigned char streamNumber;
					MafiaNet::Reliability reliability;
					// ___________________


					bitStream.IgnoreBits(8); // Ignore ID_TIMESTAMP
					bitStream.Read(receivedTime);
					bitStream.Read(packetId);
					bitStream.Read(receivedPacketNumber);
					bitStream.Read(streamNumber);
					bitStream.Read(reliability);
					if (packetId == ID_USER_PACKET_ENUM + 1)
					{

						if (reliability == MafiaNet::Reliability::UnreliableSequenced)
							type = "UnreliableSequenced";
						else if (reliability == MafiaNet::Reliability::ReliableOrdered)
							type = "ReliableOrdered";
						else
							type = "ReliableSequenced";
					}
					// ___________________
#endif


					if (internalPacket->orderingIndex == orderedReadIndex[internalPacket->orderingChannel])
					{
						// Has current ordering index
						if (internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
						    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced)
						{
							// Is sequenced
							if (IsOlderOrderedPacket(internalPacket->sequencingIndex, highestSequencedReadIndex[internalPacket->orderingChannel]) == false)
							{
								// Expected or highest known value

#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
								if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
								{
									fprintf(fp, "Returning %i, %s by fallthrough. OI=%i. SI=%i.\n", receivedPacketNumber, type, internalPacket->orderingIndex.val, internalPacket->sequencingIndex);
									fflush(fp);
								}

								if (packetId == ID_USER_PACKET_ENUM + 1)
								{
									if (receivedPacketNumber < packetNumber)
									{
										if (fp)
										{
											fprintf(fp, "Out of order packet from fallthrough! Expecting %i got %i\n", receivedPacketNumber, packetNumber);
											fflush(fp);
										}
									}
									packetNumber = receivedPacketNumber + 1;
								}
#endif
								// Update highest sequence
								// 6/26/2012 - Did not have the +1 in the next statement
								// Means a duplicated MafiaNet::Reliability::ReliableSequenced or MafiaNet::Reliability::UnreliableSequenced packet would be returned to the user
								highestSequencedReadIndex[internalPacket->orderingChannel] = internalPacket->sequencingIndex + (OrderingIndexType) 1;

								// Fallthrough, returned to user below
							}
							else
							{
#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
								if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
								{
									fprintf(fp, "Discarding %i, %s late sequenced. OI=%i. SI=%i.\n", receivedPacketNumber, type, internalPacket->orderingIndex.val, internalPacket->sequencingIndex);
									fflush(fp);
								}
#endif

#ifdef LOG_TRIVIAL_NOTIFICATIONS
								for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
									messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Sequenced rejected: lower than highest known value", BYTES_TO_BITS(length), systemAddress, false);
#endif

								// Lower than highest known value
								FreeInternalPacketData(internalPacket);
								ReleaseToInternalPacketPool(internalPacket);

								goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
							}
						}
						else
						{
							// Push to output buffer immediately
							bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));
							outputQueue.push_back(internalPacket);

#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
							if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
							{
								fprintf(fp, "outputting immediate %i, %s. OI=%i. SI=%i.", receivedPacketNumber, type, internalPacket->orderingIndex.val, internalPacket->sequencingIndex);
								if (orderingHeaps[internalPacket->orderingChannel].empty())
									fprintf(fp, "heap empty\n");
								else
									fprintf(fp, "heap head=%i\n", orderingHeaps[internalPacket->orderingChannel].front().packet->orderingIndex.val);

								if (receivedPacketNumber < packetNumber)
								{
									if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
									{
										fprintf(fp, "Out of order packet arrived! Expecting %i got %i\n", receivedPacketNumber, packetNumber);
										fflush(fp);
									}
								}
								packetNumber = receivedPacketNumber + 1;

								fflush(fp);
							}
#endif

							orderedReadIndex[internalPacket->orderingChannel]++;
							highestSequencedReadIndex[internalPacket->orderingChannel] = 0;

							// Return off heap until order lost
							const unsigned char channel = internalPacket->orderingChannel;
							while (!orderingHeaps[channel].empty())
							{
								// A buffered packet older than the read index is a second copy of a message already
								// delivered. It sorts first, so leaving it would stop this loop for good and stall the
								// channel: drop it, as a late arrival is dropped. A re-split copy sent after an MTU
								// step-down used to be one (#93); ResolveReSplitCopy now discards those before they get
								// here, so this is a backstop.
								if (IsOlderOrderedPacket(orderingHeaps[channel].front().packet->orderingIndex, orderedReadIndex[channel]))
								{
									InternalPacket *stale = HeapPop(orderingHeaps[channel]);
									bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(timeRead, BITS_TO_BYTES(stale->dataBitLength));
									FreeInternalPacketData(stale);
									ReleaseToInternalPacketPool(stale);
									continue;
								}
								if (orderingHeaps[channel].front().packet->orderingIndex != orderedReadIndex[channel])
									break;
								internalPacket = HeapPop(orderingHeaps[channel]);

#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
								BitStream bitStream2(internalPacket->data, BITS_TO_BYTES(internalPacket->dataBitLength), false);
								bitStream2.IgnoreBits(8); // Ignore ID_TIMESTAMP
								bitStream2.Read(receivedTime);
								bitStream2.IgnoreBits(8); // Ignore ID_USER_ENUM+1
								bitStream2.Read(receivedPacketNumber);
								bitStream2.Read(streamNumber);
								bitStream2.Read(reliability);
								char *type = "UNDEFINED";
								if (reliability == MafiaNet::Reliability::UnreliableSequenced)
									type = "UnreliableSequenced";
								else if (reliability == MafiaNet::Reliability::ReliableOrdered)
									type = "ReliableOrdered";

								if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
								{
									fprintf(fp, "Heap pop %i, %s. OI=%i. SI=%i.\n", receivedPacketNumber, type, internalPacket->orderingIndex.val, internalPacket->sequencingIndex);
									fflush(fp);

									if (receivedPacketNumber < packetNumber)
									{
										if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
										{
											fprintf(fp, "Out of order packet from heap! Expecting %i got %i\n", receivedPacketNumber, packetNumber);
											fflush(fp);
										}
									}
									packetNumber = receivedPacketNumber + 1;
								}
#endif

								bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));
								outputQueue.push_back(internalPacket);

								if (internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered)
								{
									orderedReadIndex[internalPacket->orderingChannel]++;
								}
								else
								{
									highestSequencedReadIndex[internalPacket->orderingChannel] = internalPacket->sequencingIndex;
								}
							}

							// Done
							goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
						}
					}
					else if (IsOlderOrderedPacket(internalPacket->orderingIndex, orderedReadIndex[internalPacket->orderingChannel]) == false)
					{
						// internalPacket->_orderingIndex is greater
						// If a message has a greater ordering index, and is sequenced or ordered, buffer it
						// Sequenced has a lower heap weight, ordered has max sequenced weight

						// Keep orderedHoleCount count small
						if (orderingHeaps[internalPacket->orderingChannel].empty())
							heapIndexOffsets[internalPacket->orderingChannel] = orderedReadIndex[internalPacket->orderingChannel];

						reliabilityHeapWeightType orderedHoleCount = internalPacket->orderingIndex - heapIndexOffsets[internalPacket->orderingChannel];
						reliabilityHeapWeightType weight = orderedHoleCount * 1048576;
						if (internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
						    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced)
							weight += internalPacket->sequencingIndex;
						else
							weight += (1048576 - 1);
						HeapPush(orderingHeaps[internalPacket->orderingChannel], orderingHeapNextSeq[internalPacket->orderingChannel], weight, internalPacket);

#ifdef PRINT_TO_FILE_RELIABLE_ORDERED_TEST
						if (packetId == ID_USER_PACKET_ENUM + 1 && fp)
						{
							fprintf(fp, "Heap push %i, %s, weight=%" PRINTF_64_BIT_MODIFIER "u. OI=%i. waiting on %i. SI=%i.\n", receivedPacketNumber, type, weight, internalPacket->orderingIndex.val, orderedReadIndex[internalPacket->orderingChannel].val, internalPacket->sequencingIndex);
							fflush(fp);
						}
#endif

#ifdef LOG_TRIVIAL_NOTIFICATIONS
						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Larger number ordered packet leaving holes", BYTES_TO_BITS(length), systemAddress, false);
#endif

						// Buffered, nothing to do
						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
					else
					{
						// Out of order
						FreeInternalPacketData(internalPacket);
						ReleaseToInternalPacketPool(internalPacket);

#ifdef LOG_TRIVIAL_NOTIFICATIONS
						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
							messageHandlerList[messageHandlerIndex]->OnReliabilityLayerNotification("Rejected older resend", BYTES_TO_BITS(length), systemAddress, false);
#endif

						// Ignored, nothing to do
						goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
					}
				}

				bpsMetrics[(int) USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

				// Nothing special about this packet.  Add it to the output queue
				outputQueue.push_back(internalPacket);

				internalPacket = 0;
			}

			// Used for a goto to jump to the resendNext packet immediately

		CONTINUE_SOCKET_DATA_PARSE_LOOP:
			// Parse the bitstream to create an internal packet
			internalPacket = CreateInternalPacketFromBitStream(&socketData, timeRead);
		}
	}

	// #med - review --- is this correct to not increase in error cases?
	receivePacketCount++;

	return true;
}

//-------------------------------------------------------------------------------------------------------
// This gets an end-user packet already parsed out. Returns number of BITS put into the buffer
//-------------------------------------------------------------------------------------------------------
BitSize_t ReliabilityLayer::Receive(unsigned char **data)
{
	InternalPacket *internalPacket;

	if (!outputQueue.empty())
	{
		internalPacket = outputQueue.front();
		outputQueue.pop_front();

		BitSize_t bitLength;
		*data = internalPacket->data;
		bitLength = internalPacket->dataBitLength;
		ReleaseToInternalPacketPool(internalPacket);
		return bitLength;
	}

	else
	{
		return 0;
	}
}

//-------------------------------------------------------------------------------------------------------
// Puts data on the send queue
// bitStream contains the data to send
// priority is what priority to send the data at
// reliability is what reliability to use
// ordering channel is from 0 to 255 and specifies what stream to use
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::Send(char *data, BitSize_t numberOfBitsToSend, MafiaNet::Priority priority, MafiaNet::Reliability reliability, unsigned char orderingChannel, bool makeDataCopy, int MTUSize, CCTimeType currentTime, uint32_t receipt)
{
#ifdef _DEBUG
	RakAssert(!((unsigned int) reliability >= MafiaNet::NUMBER_OF_RELIABILITIES || (int) reliability < 0));
	RakAssert(!((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0));
	RakAssert(!(orderingChannel >= NUMBER_OF_ORDERED_STREAMS));
	RakAssert(numberOfBitsToSend > 0);
#endif

#if CC_TIME_TYPE_BYTES == 4
	currentTime /= 1000;
#endif

	(void) MTUSize;

	//	int a = BITS_TO_BYTES(numberOfBitsToSend);

	// Fix any bad parameters
	if (reliability > MafiaNet::Reliability::ReliableOrderedWithAckReceipt || (int) reliability < 0)
		reliability = MafiaNet::Reliability::Reliable;

	if ((int) priority > (int) MafiaNet::NUMBER_OF_PRIORITIES || (int) priority < 0)
		priority = MafiaNet::Priority::High;

	if (orderingChannel >= NUMBER_OF_ORDERED_STREAMS)
		orderingChannel = 0;

	unsigned int numberOfBytesToSend = (unsigned int) BITS_TO_BYTES(numberOfBitsToSend);
	if (numberOfBitsToSend == 0)
	{
		return false;
	}
	InternalPacket *internalPacket = AllocateFromInternalPacketPool();
	if (internalPacket == 0)
	{
		return false; // Out of memory
	}

	bpsMetrics[(int) USER_MESSAGE_BYTES_PUSHED].Push1(currentTime, numberOfBytesToSend);

	internalPacket->creationTime = currentTime;

	if (makeDataCopy)
	{
		AllocInternalPacketData(internalPacket, numberOfBytesToSend, true);
		//internalPacket->data = (unsigned char*) std::malloc(numberOfBytesToSend);
		memcpy(internalPacket->data, data, numberOfBytesToSend);
	}
	else
	{
		// Allocated the data elsewhere, delete it in here
		//internalPacket->data = ( unsigned char* ) data;
		AllocInternalPacketData(internalPacket, (unsigned char *) data);
	}

	internalPacket->dataBitLength = numberOfBitsToSend;
	internalPacket->messageInternalOrder = internalOrderIndex++;
	internalPacket->priority = priority;
	internalPacket->reliability = reliability;
	internalPacket->sendReceiptSerial = receipt;

	// Calculate if I need to split the packet
	//	int headerLength = BITS_TO_BYTES( GetMessageHeaderLengthBits( internalPacket, true ) );

	unsigned int maxDataSizeBytes = GetMaxDatagramSizeExcludingMessageHeaderBytes() - BITS_TO_BYTES(GetMaxMessageHeaderLengthBits());

	bool splitPacket = numberOfBytesToSend > maxDataSizeBytes;

	// If a split packet, we might have to upgrade the reliability
	if (splitPacket)
	{
		// Split packets cannot be unreliable, in case that one part doesn't arrive and the whole cannot be reassembled.
		// One part could not arrive either due to packetloss or due to unreliable discard
		if (internalPacket->reliability == MafiaNet::Reliability::Unreliable)
			internalPacket->reliability = MafiaNet::Reliability::Reliable;
		else if (internalPacket->reliability == MafiaNet::Reliability::UnreliableWithAckReceipt)
			internalPacket->reliability = MafiaNet::Reliability::ReliableWithAckReceipt;
		else if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced)
			internalPacket->reliability = MafiaNet::Reliability::ReliableSequenced;
		//		else if (internalPacket->reliability==UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT)
		//			internalPacket->reliability=RELIABLE_SEQUENCED_WITH_ACK_RECEIPT;
	}

	//	++sendMessageNumberIndex;

	if (internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced
	    //		||
	    //		internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT ||
	    //		internalPacket->reliability == UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
	)
	{
		// Assign the sequence stream and index
		internalPacket->orderingChannel = orderingChannel;
		internalPacket->orderingIndex = orderedWriteIndex[orderingChannel];
		internalPacket->sequencingIndex = sequencedWriteIndex[orderingChannel]++;

		// This packet supersedes all other sequenced packets on the same ordering channel
		// Delete all packets in all send lists that are sequenced and on the same ordering channel
		// UPDATE:
		// Disabled.  We don't have enough info to consistently do this.  Sometimes newer data does supercede
		// older data such as with constantly declining health, but not in all cases.
		// For example, with sequenced unreliable sound packets just because you send a newer one doesn't mean you
		// don't need the older ones because the odds are they will still arrive in order
		/*
		for (int i=0; i < MafiaNet::NUMBER_OF_PRIORITIES; i++)
		{
		DeleteSequencedPacketsInList(orderingChannel, sendQueue[i]);
		}
		*/
	}
	else if (internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered || internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
	{
		// Assign the ordering channel and index
		internalPacket->orderingChannel = orderingChannel;
		internalPacket->orderingIndex = orderedWriteIndex[orderingChannel]++;
		sequencedWriteIndex[orderingChannel] = 0;
	}

	if (splitPacket) // If it uses a secure header it will be generated here
	{
		// Split the packet and add its fragments to the send list.
		//InternalPacket packetCopy;
		//memcpy(&packetCopy, internalPacket, sizeof(InternalPacket));
		//sendPacketSet[priority].CancelWriteLock(internalPacket);
		//SplitPacket( &packetCopy, MTUSize );
		SplitPacket(internalPacket);
		//delete[] packetCopy.data;
		return true;
	}

	RakAssert(internalPacket->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
	AddToUnreliableLinkedList(internalPacket);

	RakAssert(internalPacket->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
	RakAssert(internalPacket->messageNumberAssigned == false);
	const reliabilityHeapWeightType weight = GetNextWeight((int) internalPacket->priority);
	HeapPush(outgoingPacketBuffer, outgoingPacketBufferNextSeq, weight, internalPacket);
	RakAssert(outgoingPacketBuffer.empty() || outgoingPacketBuffer.front().packet->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
	statistics.messageInSendBuffer[(int) internalPacket->priority]++;
	statistics.bytesInSendBuffer[(int) internalPacket->priority] += (double) BITS_TO_BYTES(internalPacket->dataBitLength);

	//	sendPacketSet[priority].WriteUnlock();
	return true;
}
//-------------------------------------------------------------------------------------------------------
// Run this once per game cycle.  Handles internal lists and actually does the send
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::Update(RakNetSocket2 *s, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
                              unsigned bitsPerSecondLimit,
                              std::vector<PluginInterface2 *> &messageHandlerList,
                              RakNetRandom *rnr,
                              BitStream &updateBitStream)
{
	UpdateInternal(s, systemAddress, MTUSize, time, bitsPerSecondLimit, messageHandlerList, rnr, updateBitStream, false);
}

void ReliabilityLayer::UpdateAndForceACKs(RakNetSocket2 *s, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
                                          unsigned bitsPerSecondLimit,
                                          std::vector<PluginInterface2 *> &messageHandlerList,
                                          RakNetRandom *rnr,
                                          BitStream &updateBitStream)
{
	UpdateInternal(s, systemAddress, MTUSize, time, bitsPerSecondLimit, messageHandlerList, rnr, updateBitStream, true);
}

void ReliabilityLayer::UpdateInternal(RakNetSocket2 *s, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
                                      unsigned bitsPerSecondLimit,
                                      std::vector<PluginInterface2 *> &messageHandlerList,
                                      RakNetRandom *rnr,
                                      BitStream &updateBitStream, bool forceSendACKs)
{
	(void) MTUSize;

	[[maybe_unused]] MafiaNet::TimeMS timeMs; // only read by the _DEBUG simulated-lag queue below
#if CC_TIME_TYPE_BYTES == 4
	time /= 1000;
	timeMs = time;
#else
	timeMs = (MafiaNet::TimeMS)(time / (CCTimeType) 1000);
#endif

#ifdef _DEBUG
	while (!delayList.empty())
	{
		if (delayList.front()->sendTime <= timeMs)
		{
			DataAndTime *dat = delayList.front();
			delayList.pop_front();
			//			SocketLayer::SendTo( dat->s, dat->data, dat->length, systemAddress);

			RNS2_SendParameters bsp;
			bsp.data = (char *) dat->data;
			bsp.length = dat->length;
			bsp.systemAddress = systemAddress;
			dat->s->Send(&bsp);

			delete dat;
		}
		else
		{
			break;
		}
	}
#endif

	// This line is necessary because the timer isn't accurate
	if ((!forceSendACKs) && (time <= lastUpdateTime))
	{
		// Always set the last time in case of overflow
		lastUpdateTime = time;
		return;
	}

	CCTimeType timeSinceLastTick = time - lastUpdateTime;
	lastUpdateTime = time;
#if CC_TIME_TYPE_BYTES == 4
	if (timeSinceLastTick > 100)
		timeSinceLastTick = 100;
#else
	if (timeSinceLastTick > 100000)
		timeSinceLastTick = 100000;
#endif

	if (unreliableTimeout > 0)
	{
		if (timeSinceLastTick >= timeToNextUnreliableCull)
		{
			if (unreliableLinkedListHead)
			{
				// Cull out all unreliable messages that have exceeded the timeout
				InternalPacket *cur = unreliableLinkedListHead;
				InternalPacket *end = unreliableLinkedListHead->unreliablePrev;

				for (;;)
				{
					if (time > cur->creationTime + (CCTimeType) unreliableTimeout)
					{
						// Flag invalid, and clear the memory. Still needs to be removed from the sendPacketSet later
						// This fixes a problem where a remote system disconnects, but we don't know it yet, and memory consumption increases to a huge value
						FreeInternalPacketData(cur);
						cur->data = 0;
						InternalPacket *next = cur->unreliableNext;
						RemoveFromUnreliableLinkedList(cur);

						if (cur == end)
							break;

						cur = next;
					}
					else
					{
						// 						if (cur==end)
						// 							break;
						//
						// 						cur=cur->unreliableNext;

						// They should be inserted in-order, so no need to iterate past the first failure
						break;
					}
				}
			}

			timeToNextUnreliableCull = unreliableTimeout / (CCTimeType) 2;
		}
		else
		{
			timeToNextUnreliableCull -= timeSinceLastTick;
		}
	}


	// Due to thread vagarities and the way I store the time to avoid slow calls to MafiaNet::GetTime
	// time may be less than lastAck
#if CC_TIME_TYPE_BYTES == 4
	if (statistics.messagesInResendBuffer != 0 && AckTimeout(time))
#else
	if (statistics.messagesInResendBuffer != 0 && AckTimeout(MafiaNet::TimeMS(time / (CCTimeType) 1000)))
#endif
	{
		// SHOW - dead connection
		// We've waited a very long time for a reliable packet to get an ack and it never has
		deadConnection = true;
		return;
	}

	if (forceSendACKs || congestionManager.ShouldSendACKs(time, timeSinceLastTick))
	{
		SendACKs(s, systemAddress, time, rnr, updateBitStream);
	}

	constexpr unsigned microsPerUnit = CC_TIME_TYPE_BYTES == 4 ? 1000 : 1;
	datagramGaps.Update(uint64_t{time} * microsPerUnit, NAKs);
	if (NAKs.Size() > 0)
	{
		updateBitStream.Reset();
		DatagramHeaderFormat dhfNAK;
		dhfNAK.isNAK = true;
		dhfNAK.isACK = false;
		dhfNAK.isPacketPair = false;
		dhfNAK.Serialize(&updateBitStream);
		NAKs.Serialize(&updateBitStream, GetMaxDatagramSizeExcludingMessageHeaderBits(), true);
		SendBitStream(s, systemAddress, &updateBitStream, rnr, time);
	}

	DatagramHeaderFormat dhf;
	dhf.needsBAndAs = congestionManager.GetIsInSlowStart();
	dhf.isContinuousSend = bandwidthExceededStatistic;
	// 	bandwidthExceededStatistic=sendPacketSet[0].IsEmpty()==false ||
	// 		sendPacketSet[1].IsEmpty()==false ||
	// 		sendPacketSet[2].IsEmpty()==false ||
	// 		sendPacketSet[3].IsEmpty()==false;
	bandwidthExceededStatistic = !outgoingPacketBuffer.empty();

	const bool hasDataToSendOrResend = IsResendQueueEmpty() == false || bandwidthExceededStatistic;
	RakAssert(MafiaNet::NUMBER_OF_PRIORITIES == 4);
	congestionManager.Update(time, hasDataToSendOrResend);

	// Pace new payload over update ticks. Sending one whole packet may put the
	// budget in debt; later ticks repay it. Idle time does not bank a burst.
	if (bitsPerSecondLimit)
	{
		constexpr int microsPerTick = CC_TIME_TYPE_BYTES == 4 ? 1000 : 1;
		const auto elapsedMicros = static_cast<int64_t>(timeSinceLastTick) * microsPerTick;
		outgoingCreditBitMicros = std::min<int64_t>(0, outgoingCreditBitMicros) + elapsedMicros * bitsPerSecondLimit;
	}
	else
		outgoingCreditBitMicros = 0;
	statistics.BPSLimitByOutgoingBandwidthLimit = BITS_TO_BYTES(static_cast<uint64_t>(bitsPerSecondLimit));
	statistics.BPSLimitByCongestionControl = congestionManager.GetBytesPerSecondLimitByCongestionControl();

	unsigned int i;
	if (time > lastBpsClear +
#if CC_TIME_TYPE_BYTES == 4
	               100
#else
	               100000
#endif
	)
	{
		for (i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++)
		{
			bpsMetrics[i].ClearExpired1(time);
		}

		lastBpsClear = time;
	}

	if (!unreliableWithAckReceiptHistory.empty())
	{
		std::size_t receiptIndex = 0;
		while (receiptIndex < unreliableWithAckReceiptHistory.size())
		{
			//if (unreliableWithAckReceiptHistory[receiptIndex].nextActionTime < time)
			if (time - unreliableWithAckReceiptHistory[receiptIndex].nextActionTime < (((CCTimeType) -1) / 2))
			{
				InternalPacket *ackReceipt = AllocateFromInternalPacketPool();
				AllocInternalPacketData(ackReceipt, 5, false);
				ackReceipt->dataBitLength = BYTES_TO_BITS(5);
				ackReceipt->data[0] = (MessageID) ID_SND_RECEIPT_LOSS;
				memcpy(ackReceipt->data + sizeof(MessageID), &unreliableWithAckReceiptHistory[receiptIndex].sendReceiptSerial, sizeof(uint32_t));
				outputQueue.push_back(ackReceipt);

				// Remove, keeping the order of the rest
				unreliableWithAckReceiptHistory.erase(unreliableWithAckReceiptHistory.begin() + receiptIndex);
			}
			else
				receiptIndex++;
		}
	}

	if (hasDataToSendOrResend == true)
	{
		InternalPacket *internalPacket;
		//		bool forceSend=false;
		bool pushedAnything;
		BitSize_t nextPacketBitLength;
		dhf.isACK = false;
		dhf.isNAK = false;
		dhf.hasBAndAS = false;
		ResetPacketsAndDatagrams();

		// In-session path-MTU black-hole detection. The handshake probed the
		// path in one direction only; a tunnel that drops large datagrams on
		// the other direction leaves the head of the resend queue burning its
		// resend budget on a packet that can never arrive. Checked only here,
		// before any packet is pushed this tick, because the step-down frees
		// queued packets and packetsToSendThisUpdate must not hold pointers to
		// them.
		if (IsResendQueueEmpty() == false)
		{
			InternalPacket *stuck = resendLinkedListHead;
			if (time - stuck->nextActionTime < (((CCTimeType) -1) / 2))
			{
				const int requiredDatagramBytes = (int) BITS_TO_BYTES(stuck->headerLength + stuck->dataBitLength) +
				                                  (int) DatagramHeaderFormat::GetDataHeaderByteLength() + UDP_HEADER_SIZE + cipherOverheadBytes;
				if (ShouldStepDownMtu(stuck->timesSent, requiredDatagramBytes, currentMtuBytes) && mtuBlackHoleEvidence.IsBlackHoleLikely())
					StepDownMtuAfterBlackHole();
			}
		}

		int transmissionBandwidth = congestionManager.GetTransmissionBandwidth(time, timeSinceLastTick, unacknowledgedBytes, dhf.isContinuousSend);
		int retransmissionBandwidth = congestionManager.GetRetransmissionBandwidth(time, timeSinceLastTick, unacknowledgedBytes, dhf.isContinuousSend);
		if (retransmissionBandwidth > 0 || transmissionBandwidth > 0)
		{
			statistics.isLimitedByCongestionControl = false;

			allDatagramSizesSoFar = 0;

			// Keep filling datagrams until we exceed retransmission bandwidth
			while ((int) BITS_TO_BYTES(allDatagramSizesSoFar) < retransmissionBandwidth)
			{
				pushedAnything = false;

				// Fill one datagram, then break
				while (IsResendQueueEmpty() == false)
				{
					internalPacket = resendLinkedListHead;
					RakAssert(internalPacket->messageNumberAssigned == true);

					//if ( internalPacket->nextActionTime < time )
					if (time - internalPacket->nextActionTime < (((CCTimeType) -1) / 2))
					{
						nextPacketBitLength = internalPacket->headerLength + internalPacket->dataBitLength;
						if (datagramSizeSoFar + nextPacketBitLength > GetMaxDatagramSizeExcludingMessageHeaderBits())
						{
							// Gathers all PushPackets()
							PushDatagram();
							break;
						}

						PopListHead(false);

						CC_DEBUG_PRINTF_2("Rs %i ", internalPacket->reliableMessageNumber.val);

						bpsMetrics[(int) USER_MESSAGE_BYTES_RESENT].Push1(time, BITS_TO_BYTES(internalPacket->dataBitLength));

						// Testing1
						// 						if (internalPacket->reliability==MafiaNet::Reliability::ReliableOrdered || internalPacket->reliability==MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
						// 							printf("RESEND reliableMessageNumber %i with datagram %i\n", internalPacket->reliableMessageNumber.val, congestionManager.GetNextDatagramSequenceNumber().val);

						// Its previous transmission went unacked: a failure, and black-hole
						// evidence if the packet would shrink at the next rung.
						mtuBlackHoleEvidence.OnResend((int) BITS_TO_BYTES(nextPacketBitLength) + (int) DatagramHeaderFormat::GetDataHeaderByteLength() + UDP_HEADER_SIZE + cipherOverheadBytes,
						                              currentMtuBytes, (uint64_t) internalPacket->lastTransmissionTime, (uint64_t) time);

						PushPacket(time, internalPacket, true); // Affects GetNewTransmissionBandwidth()
						internalPacket->timesSent++;
						congestionManager.OnResend(time, internalPacket->nextActionTime);
						internalPacket->retransmissionTime = congestionManager.GetRTOForRetransmission(internalPacket->timesSent);
						internalPacket->nextActionTime = internalPacket->retransmissionTime + time;

						pushedAnything = true;

						for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
						{
#if CC_TIME_TYPE_BYTES == 4
							messageHandlerList[messageHandlerIndex]->OnInternalPacket(internalPacket, (uint32_t) packetsToSendThisUpdateDatagramBoundaries.size() + congestionManager.GetNextDatagramSequenceNumber(), systemAddress, (MafiaNet::TimeMS) time, true);
#else
							messageHandlerList[messageHandlerIndex]->OnInternalPacket(internalPacket, (uint32_t) packetsToSendThisUpdateDatagramBoundaries.size() + congestionManager.GetNextDatagramSequenceNumber(), systemAddress, (MafiaNet::TimeMS)(time / (CCTimeType) 1000), true);
#endif
						}

						// Put the packet back into the resend list at the correct spot
						// Don't make a copy since I'm reinserting an allocated struct
						InsertPacketIntoResendList(internalPacket, time, false, false);

						// Removeme
						//						printf("Resend:%i ", internalPacket->reliableMessageNumber);
					}
					else
					{
						// Filled one datagram.
						// If the 2nd and it's time to send a datagram pair, will be marked as a pair
						PushDatagram();
						break;
					}
				}

				if (pushedAnything == false)
					break;
			}
		}
		else
		{
			statistics.isLimitedByCongestionControl = true;
		}

		if ((int) BITS_TO_BYTES(allDatagramSizesSoFar) < transmissionBandwidth)
		{
			//	printf("S+ ");
			allDatagramSizesSoFar = 0;

			// Keep filling datagrams until we exceed transmission bandwidth
			while (
			    ResendBufferOverflow() == false &&
			    ((int) BITS_TO_BYTES(allDatagramSizesSoFar) < transmissionBandwidth ||
			     // This condition means if we want to send a datagram pair, and only have one datagram buffered, exceed bandwidth to add another
			     (countdownToNextPacketPair == 0 &&
			      datagramsToSendThisUpdateIsPair.size() == 1)))
			{
				// Fill with packets until MTU is reached
				//	for ( i = 0; i < MafiaNet::NUMBER_OF_PRIORITIES; i++ )
				//	{
				pushedAnything = false;

				while (!outgoingPacketBuffer.empty() && (bitsPerSecondLimit == 0 || outgoingCreditBitMicros > 0))
				//while ( sendPacketSet[ i ].Size() )
				{
					internalPacket = outgoingPacketBuffer.front().packet;
					RakAssert(internalPacket->messageNumberAssigned == false);
					RakAssert(outgoingPacketBuffer.empty() || outgoingPacketBuffer.front().packet->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));

					// internalPacket = sendPacketSet[ i ].Peek();
					if (internalPacket->data == 0)
					{
						//sendPacketSet[ i ].Pop();
						HeapPop(outgoingPacketBuffer);
						RakAssert(outgoingPacketBuffer.empty() || outgoingPacketBuffer.front().packet->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
						statistics.messageInSendBuffer[(int) internalPacket->priority]--;
						statistics.bytesInSendBuffer[(int) internalPacket->priority] -= (double) BITS_TO_BYTES(internalPacket->dataBitLength);
						ReleaseToInternalPacketPool(internalPacket);
						continue;
					}

					internalPacket->headerLength = GetMessageHeaderLengthBits(internalPacket);
					nextPacketBitLength = internalPacket->headerLength + internalPacket->dataBitLength;
					if (datagramSizeSoFar + nextPacketBitLength > GetMaxDatagramSizeExcludingMessageHeaderBits())
					{
						// Hit MTU. May still push packets if smaller ones exist at a lower priority
						RakAssert(datagramSizeSoFar != 0);
						RakAssert(internalPacket->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
						break;
					}

					bool isReliable;
					if (internalPacket->reliability == MafiaNet::Reliability::Reliable ||
					    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
					    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
					    internalPacket->reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
					    //						internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT  ||
					    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
						isReliable = true;
					else
						isReliable = false;

					//sendPacketSet[ i ].Pop();
					HeapPop(outgoingPacketBuffer);
					RakAssert(outgoingPacketBuffer.empty() || outgoingPacketBuffer.front().packet->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
					RakAssert(internalPacket->messageNumberAssigned == false);
					statistics.messageInSendBuffer[(int) internalPacket->priority]--;
					statistics.bytesInSendBuffer[(int) internalPacket->priority] -= (double) BITS_TO_BYTES(internalPacket->dataBitLength);
					if (isReliable
					    /*
						I thought about this and agree that UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT and RELIABLE_SEQUENCED_WITH_ACK_RECEIPT is not useful unless you also know if the message was discarded.

						The problem is that internally, message numbers are only assigned to reliable messages, because message numbers are only used to discard duplicate message receipt and only reliable messages get sent more than once. However, without message numbers getting assigned and transmitted, there is no way to tell the sender about which messages were discarded. In fact, in looking this over I realized that UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT introduced a bug, because the remote system assumes all message numbers are used (no holes). With that send type, on packetloss, a permanent hole would have been created which eventually would cause the system to discard all further packets.

						So I have two options. Either do not support ack receipts when sending sequenced, or write complex and major new systems. UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT would need to send the message ID number on a special channel which allows for non-delivery. And both of them would need to have a special range list to indicate which message numbers were not delivered, so when acks are sent that can be indicated as well. A further problem is that the ack itself can be lost - it is possible that the message can arrive but be discarded, yet the ack is lost. On resend, the resent message would be ignored as duplicate, and you'd never get the discard message either (unless I made a special buffer for that case too). 
*/
					    //						||
					    // If needs an ack receipt, keep the internal packet around in the list
					    //						internalPacket->reliability == MafiaNet::Reliability::UnreliableWithAckReceipt ||
					    //						internalPacket->reliability == UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
					)
					{
						internalPacket->messageNumberAssigned = true;
						internalPacket->reliableMessageNumber = sendReliableMessageNumberIndex;
						internalPacket->retransmissionTime = congestionManager.GetRTOForRetransmission(internalPacket->timesSent + 1);
						internalPacket->nextActionTime = internalPacket->retransmissionTime + time;
#if CC_TIME_TYPE_BYTES == 4
						const CCTimeType threshhold = 10000;
#else
						const CCTimeType threshhold = 10000000;
#endif
						if (internalPacket->nextActionTime - time > threshhold)
						{
							//								int a=5;
							RakAssert(time - internalPacket->nextActionTime < threshhold);
						}
						//resendTree.Insert( internalPacket->reliableMessageNumber, internalPacket);
						if (resendBuffer[internalPacket->reliableMessageNumber & (uint32_t) RESEND_BUFFER_ARRAY_MASK] != 0)
						{
							//								bool overflow = ResendBufferOverflow();
							RakAssert(0);
						}
						resendBuffer[internalPacket->reliableMessageNumber & (uint32_t) RESEND_BUFFER_ARRAY_MASK] = internalPacket;
						statistics.messagesInResendBuffer++;
						statistics.bytesInResendBuffer += BITS_TO_BYTES(internalPacket->dataBitLength);

						//		printf("pre:%i ", unacknowledgedBytes);

						InsertPacketIntoResendList(internalPacket, time, true, isReliable);


						//		printf("post:%i ", unacknowledgedBytes);
						sendReliableMessageNumberIndex++;
					}
					else if (internalPacket->reliability == MafiaNet::Reliability::UnreliableWithAckReceipt)
					{
						unreliableWithAckReceiptHistory.push_back(UnreliableWithAckReceiptNode(
						    congestionManager.GetNextDatagramSequenceNumber() + (uint32_t) packetsToSendThisUpdateDatagramBoundaries.size(),
						    internalPacket->sendReceiptSerial,
						    congestionManager.GetRTOForRetransmission(internalPacket->timesSent + 1) + time));
					}

					// If isReliable is false, the packet and its contents will be added to a list to be freed in ClearPacketsAndDatagrams
					// However, the internalPacket structure will remain allocated and be in the resendBuffer list if it requires a receipt
					const auto payloadBytes = BITS_TO_BYTES(internalPacket->dataBitLength);
					bpsMetrics[(int) USER_MESSAGE_BYTES_SENT].Push1(time, payloadBytes);
					if (bitsPerSecondLimit)
						outgoingCreditBitMicros -= static_cast<int64_t>(payloadBytes) * 8 * 1000000;

					// Testing1
					// 					if (internalPacket->reliability==MafiaNet::Reliability::ReliableOrdered || internalPacket->reliability==MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
					// 						printf("SEND reliableMessageNumber %i in datagram %i\n", internalPacket->reliableMessageNumber.val, congestionManager.GetNextDatagramSequenceNumber().val);

					PushPacket(time, internalPacket, isReliable);
					internalPacket->timesSent++;

					for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
					{
#if CC_TIME_TYPE_BYTES == 4
						messageHandlerList[messageHandlerIndex]->OnInternalPacket(internalPacket, (uint32_t) packetsToSendThisUpdateDatagramBoundaries.size() + congestionManager.GetNextDatagramSequenceNumber(), systemAddress, (MafiaNet::TimeMS) time, true);
#else
						messageHandlerList[messageHandlerIndex]->OnInternalPacket(internalPacket, (uint32_t) packetsToSendThisUpdateDatagramBoundaries.size() + congestionManager.GetNextDatagramSequenceNumber(), systemAddress, (MafiaNet::TimeMS)(time / (CCTimeType) 1000), true);
#endif
					}
					pushedAnything = true;

					if (ResendBufferOverflow())
						break;
				}
				//	if (ResendBufferOverflow())
				//		break;
				//	}z

				// No datagrams pushed?
				if (datagramSizeSoFar == 0)
					break;

				// Filled one datagram.
				// If the 2nd and it's time to send a datagram pair, will be marked as a pair
				PushDatagram();
			}
		}


#if defined(__linux__)
		// Coalesce this update's datagrams (all to the same peer) into one
		// sendmmsg. Loop-local: flushed at the single loop exit below.
		RNS2SendBatch sendBatch(s, systemAddress);
#endif
		for (unsigned int datagramIndex = 0; datagramIndex < packetsToSendThisUpdateDatagramBoundaries.size(); datagramIndex++)
		{
			if (datagramIndex > 0)
				dhf.isContinuousSend = true;
			MessageNumberNode *messageNumberNode = 0;
			dhf.datagramNumber = congestionManager.GetAndIncrementNextDatagramSequenceNumber();
			dhf.isPacketPair = datagramsToSendThisUpdateIsPair[datagramIndex];

			//printf("%p pushing datagram %i\n", this, dhf.datagramNumber.val);

			bool isSecondOfPacketPair = dhf.isPacketPair && datagramIndex > 0 && datagramsToSendThisUpdateIsPair[datagramIndex - 1];
			unsigned int msgIndex, msgTerm;
			if (datagramIndex == 0)
			{
				msgIndex = 0;
				msgTerm = packetsToSendThisUpdateDatagramBoundaries[0];
			}
			else
			{
				msgIndex = packetsToSendThisUpdateDatagramBoundaries[datagramIndex - 1];
				msgTerm = packetsToSendThisUpdateDatagramBoundaries[datagramIndex];
			}

			// More accurate time to reset here
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
			dhf.sourceSystemTime = MafiaNet::GetTimeUS();
#endif
			updateBitStream.Reset();
			dhf.Serialize(&updateBitStream);
			const BitSize_t datagramHeaderBits = updateBitStream.GetNumberOfBitsUsed();
			CC_DEBUG_PRINTF_2("S%i ", dhf.datagramNumber.val);

			while (msgIndex < msgTerm)
			{
				// If reliable or needs receipt
				if (packetsToSendThisUpdate[msgIndex]->reliability != MafiaNet::Reliability::Unreliable &&
				    packetsToSendThisUpdate[msgIndex]->reliability != MafiaNet::Reliability::UnreliableSequenced)
				{
					if (messageNumberNode == 0)
					{
						messageNumberNode = AddFirstToDatagramHistory(dhf.datagramNumber, packetsToSendThisUpdate[msgIndex]->reliableMessageNumber, time);
					}
					else
					{
						messageNumberNode = AddSubsequentToDatagramHistory(messageNumberNode, packetsToSendThisUpdate[msgIndex]->reliableMessageNumber);
					}
				}

				RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
				WriteToBitStreamFromInternalPacket(&updateBitStream, packetsToSendThisUpdate[msgIndex], time);
				RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
				msgIndex++;
			}

			if (isSecondOfPacketPair)
			{
				// Pad to size of first datagram
				RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
				updateBitStream.PadWithZeroToByteLength(datagramSizesInBytes[datagramIndex - 1]);
				RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
			}

			if (messageNumberNode == 0)
			{
				// Unreliable, add dummy node
				AddFirstToDatagramHistory(dhf.datagramNumber, time);
			}
			// The node just pushed is this datagram's: record its size, estimated the way the black-hole
			// check estimates a packet's (DatagramHeaderFormat::GetDataHeaderByteLength() for the header),
			// so that an ack for it compares like with like against failed packets.
			datagramHistory.back().wireBytes = (int) BITS_TO_BYTES(updateBitStream.GetNumberOfBitsUsed() - datagramHeaderBits) +
			                                   (int) DatagramHeaderFormat::GetDataHeaderByteLength() + UDP_HEADER_SIZE + cipherOverheadBytes;

			// Store what message ids were sent with this datagram
			//	datagramMessageIDTree.Insert(dhf.datagramNumber,idList);

			congestionManager.OnSendBytes(time, UDP_HEADER_SIZE + DatagramHeaderFormat::GetDataHeaderByteLength());

#if defined(__linux__)
			SendBitStream(s, systemAddress, &updateBitStream, rnr, time, &sendBatch);
#else
			SendBitStream(s, systemAddress, &updateBitStream, rnr, time);
#endif

			bandwidthExceededStatistic = !outgoingPacketBuffer.empty();
			// 			bandwidthExceededStatistic=sendPacketSet[0].IsEmpty()==false ||
			// 				sendPacketSet[1].IsEmpty()==false ||
			// 				sendPacketSet[2].IsEmpty()==false ||
			// 				sendPacketSet[3].IsEmpty()==false;


			if (bandwidthExceededStatistic == true)
				timeOfLastContinualSend = time;
			else
				timeOfLastContinualSend = 0;
		}

#if defined(__linux__)
		// Single flush point for the whole update's datagrams.
		sendBatch.Flush();
#endif

		ClearPacketsAndDatagrams();

		// Any data waiting to send after attempting to send, then bandwidth is exceeded
		bandwidthExceededStatistic = !outgoingPacketBuffer.empty();
		// 		bandwidthExceededStatistic=sendPacketSet[0].IsEmpty()==false ||
		// 			sendPacketSet[1].IsEmpty()==false ||
		// 			sendPacketSet[2].IsEmpty()==false ||
		// 			sendPacketSet[3].IsEmpty()==false;
	}


	statistics.isLimitedByOutgoingBandwidthLimit = bitsPerSecondLimit != 0 && outgoingCreditBitMicros <= 0 && !outgoingPacketBuffer.empty();

	// Keep on top of deleting old unreliable split packets so they don't clog the list.
	//DeleteOldUnreliableSplitPackets( time );
}

//-------------------------------------------------------------------------------------------------------
// Writes a bitstream to the socket
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SendBitStream(RakNetSocket2 *s, SystemAddress &systemAddress, MafiaNet::BitStream *bitStream, RakNetRandom *rnr, CCTimeType currentTime, RNS2SendBatch *sendBatch)
{
	(void) systemAddress;
	(void) rnr;
	(void) sendBatch;

	unsigned int length;

	length = (unsigned int) bitStream->GetNumberOfBytesUsed();

	// The plaintext datagram fits the budget Reset() derived; sealing adds at most
	// cipherOverheadBytes, which that budget reserved.
	RakAssert(length <= congestionManager.GetMTU());

	// Mode Required without keys: nothing leaves in clear. The datagram is dropped
	// as if lost; reliable messages in it are resent once keys exist, or time out.
	if (encryptionMode == EncryptionMode::Required && !datagramCipher.HasKeys())
	{
		droppedKeylessSends++;
		return;
	}

	// Seal before anything below can queue, delay or transmit the datagram, so the
	// simulator's delay list and the sendmmsg batch only ever see ciphertext.
	if (datagramCipher.HasKeys())
	{
		unsigned char plaintext[MAXIMUM_MTU_SIZE];
		RakAssert(length <= sizeof(plaintext));
		if (length > sizeof(plaintext))
			return;
		memcpy(plaintext, bitStream->GetData(), length);
		// Seal writes up to kMaxOverhead bytes past the datagram; make the buffer
		// hold them (plus the partial byte GetNumberOfBytesUsed rounds up).
		bitStream->AddBitsAndReallocate(BYTES_TO_BITS((BitSize_t) DatagramCipher::kMaxOverhead + 1));
		const int capacity = (int) (bitStream->GetNumberOfBitsAllocated() / 8);
		const int sealed = datagramCipher.Seal(plaintext, (int) length, (uint8_t *) bitStream->GetData(), capacity);
		RakAssert(sealed >= 0);
		if (sealed < 0)
			return; // never fall back to sending plaintext
		length = (unsigned int) sealed;
	}

#ifdef _DEBUG
	if (packetloss > 0.0)
	{
		if (frandomMT() < packetloss)
			return;
	}

	if (minExtraPing > 0 || extraPingVariance > 0)
	{
#ifdef FLIP_SEND_ORDER_TEST
		// Flip order of sends without delaying them for testing
		DataAndTime *dat = new DataAndTime;
		memcpy(dat->data, (char *) bitStream->GetData(), length);
		dat->s = s;
		dat->length = length;
		dat->sendTime = 0;
		dat->extraSocketOptions = extraSocketOptions;
		delayList.push_front(dat);
#else
		MafiaNet::TimeMS delay = minExtraPing;
		if (extraPingVariance > 0)
			delay += (randomMT() % extraPingVariance);
		if (delay > 0)
		{
			DataAndTime *dat = new DataAndTime;
			memcpy(dat->data, (char *) bitStream->GetData(), length);
			dat->s = s;
			dat->length = length;
			dat->sendTime = MafiaNet::GetTimeMS() + delay;
			for (std::size_t i = 0; i < delayList.size(); i++)
			{
				if (dat->sendTime < delayList[i]->sendTime)
				{
					delayList.insert(delayList.begin() + i, dat);
					dat = 0;
					break;
				}
			}
			if (dat != 0)
				delayList.push_back(dat);
			return;
		}
#endif
	}
#endif

	bpsMetrics[(int) ACTUAL_BYTES_SENT].Push1(currentTime, length);

#ifdef USE_THREADED_SEND
	SendToThread::SendToThreadBlock *block = SendToThread::AllocateBlock();
	memcpy(block->data, bitStream->GetData(), length);
	block->dataWriteOffset = length;
	block->extraSocketOptions = extraSocketOptions;
	block->s = s;
	block->systemAddress = systemAddress;
	SendToThread::ProcessBlock(block);
#else
	// SocketLayer::SendTo( s, ( char* ) bitStream->GetData(), length, systemAddress);

#if defined(__linux__)
	if (sendBatch)
	{
		// Defer only the transmit: the datagram above is fully prepared
		// (encrypted, metered). The batch flushes via sendmmsg at the caller's
		// loop boundary.
		sendBatch->Add((const char *) bitStream->GetData(), (int) length);
	}
	else
#endif
	{
		RNS2_SendParameters bsp;
		bsp.data = (char *) bitStream->GetData();
		bsp.length = length;
		bsp.systemAddress = systemAddress;
		s->Send(&bsp);
	}
#endif
}

//-------------------------------------------------------------------------------------------------------
// Are we waiting for any data to be sent out or be processed by the player?
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsOutgoingDataWaiting(void)
{
	if (!outgoingPacketBuffer.empty())
		return true;

	// 	unsigned i;
	// 	for ( i = 0; i < MafiaNet::NUMBER_OF_PRIORITIES; i++ )
	// 	{
	// 		if (sendPacketSet[ i ].Size() > 0)
	// 			return true;
	// 	}

	return
	    //acknowlegements.Size() > 0 ||
	    //resendTree.IsEmpty()==false;// || outputQueue.Size() > 0 || orderingList.Size() > 0 || splitPacketChannelList.Size() > 0;
	    statistics.messagesInResendBuffer != 0;
}
bool ReliabilityLayer::AreAcksWaiting(void)
{
	return acknowlegements.Size() > 0;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::ApplyNetworkSimulator(double _packetloss, MafiaNet::TimeMS _minExtraPing, MafiaNet::TimeMS _extraPingVariance)
{
#ifndef _DEBUG
	// unused parameters
	(void) _packetloss;
	(void) _minExtraPing;
	(void) _extraPingVariance;
#endif

#ifdef _DEBUG
	packetloss = _packetloss;
	minExtraPing = _minExtraPing;
	extraPingVariance = _extraPingVariance;
	//	if (ping < (unsigned int)(minExtraPing+extraPingVariance)*2)
	//		ping=(minExtraPing+extraPingVariance)*2;
#endif
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SetSplitMessageProgressInterval(int interval)
{
	splitMessageProgressInterval = interval;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SetUnreliableTimeout(MafiaNet::TimeMS timeoutMS)
{
#if CC_TIME_TYPE_BYTES == 4
	unreliableTimeout = timeoutMS;
#else
	unreliableTimeout = (CCTimeType) timeoutMS * (CCTimeType) 1000;
#endif
}

//-------------------------------------------------------------------------------------------------------
// This will return true if we should not send at this time
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsSendThrottled(int MTUSize)
{
	(void) MTUSize;

	return false;
	//	return resendList.Size() > windowSize;

	// Disabling this, because it can get stuck here forever
	/*
	unsigned packetsWaiting;
	unsigned resendListDataSize=0;
	unsigned i;
	for (i=0; i < resendList.Size(); i++)
	{
	if (resendList[i])
	resendListDataSize+=resendList[i]->dataBitLength;
	}
	packetsWaiting = 1 + ((BITS_TO_BYTES(resendListDataSize)) / (MTUSize - UDP_HEADER_SIZE - 10)); // 10 to roughly estimate the raknet header

	return packetsWaiting >= windowSize;
	*/
}

//-------------------------------------------------------------------------------------------------------
// We lost a packet
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::UpdateWindowFromPacketloss(CCTimeType time)
{
	(void) time;
}

//-------------------------------------------------------------------------------------------------------
// Increase the window size
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::UpdateWindowFromAck(CCTimeType time)
{
	(void) time;
}

//-------------------------------------------------------------------------------------------------------
// Does what the function name says
//-------------------------------------------------------------------------------------------------------
unsigned ReliabilityLayer::RemovePacketFromResendListAndDeleteOlderReliableSequenced(const MessageNumberType messageNumber, CCTimeType time, std::vector<PluginInterface2 *> &messageHandlerList, const SystemAddress &systemAddress)
{
	(void) time;
	(void) messageNumber;
	InternalPacket *internalPacket;
	//InternalPacket *temp;
	//	MafiaNet::Reliability reliability; // What type of reliability algorithm to use with this packet
	//	unsigned char orderingChannel; // What ordering channel this packet is on, if the reliability type uses ordering channels
	//	OrderingIndexType orderingIndex; // The ID used as identification for ordering channels
	//	unsigned j;

	for (unsigned int messageHandlerIndex = 0; messageHandlerIndex < messageHandlerList.size(); messageHandlerIndex++)
	{
#if CC_TIME_TYPE_BYTES == 4
		messageHandlerList[messageHandlerIndex]->OnAck(messageNumber, systemAddress, time);
#else
		messageHandlerList[messageHandlerIndex]->OnAck(messageNumber, systemAddress, (MafiaNet::TimeMS)(time / (CCTimeType) 1000));
#endif
	}

	// Testing1
	// 	if (resendLinkedListHead)
	// 	{
	// 		InternalPacket *internalPacket = resendLinkedListHead;
	// 		do
	// 		{
	// 			internalPacket=internalPacket->resendNext;
	// 			printf("%i ", internalPacket->reliableMessageNumber.val);
	// 		} while (internalPacket!=resendLinkedListHead);
	// 		printf("\n");
	// 	}

	//	bool deleted;
	//	deleted=resendTree.Delete(messageNumber, internalPacket);
	internalPacket = resendBuffer[messageNumber & RESEND_BUFFER_ARRAY_MASK];
	// May ask to remove twice, for example resend twice, then second ack
	if (internalPacket && internalPacket->reliableMessageNumber == messageNumber)
	{
		//	ValidateResendList();
		resendBuffer[messageNumber & RESEND_BUFFER_ARRAY_MASK] = 0;
		CC_DEBUG_PRINTF_2("AckRcv %i ", messageNumber);

		statistics.messagesInResendBuffer--;
		statistics.bytesInResendBuffer -= BITS_TO_BYTES(internalPacket->dataBitLength);

		//		orderingIndex = internalPacket->orderingIndex;
		totalUserDataBytesAcked += (double) BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength);

		// Return receipt if asked for
		if (internalPacket->reliability >= MafiaNet::Reliability::ReliableWithAckReceipt &&
		    (internalPacket->splitPacketCount == 0 || internalPacket->splitPacketIndex + 1 == internalPacket->splitPacketCount))
		{
			InternalPacket *ackReceipt = AllocateFromInternalPacketPool();
			AllocInternalPacketData(ackReceipt, 5, false);
			ackReceipt->dataBitLength = BYTES_TO_BITS(5);
			ackReceipt->data[0] = (MessageID) ID_SND_RECEIPT_ACKED;
			memcpy(ackReceipt->data + sizeof(MessageID), &internalPacket->sendReceiptSerial, sizeof(internalPacket->sendReceiptSerial));
			outputQueue.push_back(ackReceipt);
		}

		bool isReliable;
		if (internalPacket->reliability == MafiaNet::Reliability::Reliable ||
		    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
		    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
		    internalPacket->reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
		    //			internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT  ||
		    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
			isReliable = true;
		else
			isReliable = false;

		RemoveFromList(internalPacket, isReliable);
		FreeInternalPacketData(internalPacket);
		ReleaseToInternalPacketPool(internalPacket);


		return 0;
	}
	else
	{
	}

	return (unsigned) -1;
}

//-------------------------------------------------------------------------------------------------------
// Acknowledge receipt of the packet with the specified messageNumber
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SendAcknowledgementPacket(const DatagramSequenceNumberType messageNumber, CCTimeType time)
{

	// REMOVEME
	// printf("%p Send ack %i\n", this, messageNumber.val);

	nextAckTimeToSend = time;
	acknowlegements.Insert(messageNumber);

	//printf("ACK_DG:%i ", messageNumber.val);

	CC_DEBUG_PRINTF_2("AckPush %i ", messageNumber);
}

//-------------------------------------------------------------------------------------------------------
// Parse an internalPacket and figure out how many header bits would be
// written.  Returns that number
//-------------------------------------------------------------------------------------------------------
BitSize_t ReliabilityLayer::GetMaxMessageHeaderLengthBits(void)
{
	InternalPacket ip;
	ip.reliability = MafiaNet::Reliability::ReliableSequenced;
	ip.splitPacketCount = 1;
	return GetMessageHeaderLengthBits(&ip);
}
//-------------------------------------------------------------------------------------------------------
BitSize_t ReliabilityLayer::GetMessageHeaderLengthBits(const InternalPacket *const internalPacket)
{
	BitSize_t bitLength;

	//	bitStream->AlignWriteToByteBoundary(); // Potentially unaligned
	//	tempChar=(unsigned char)internalPacket->reliability; bitStream->WriteBits( (const unsigned char *)&tempChar, 3, true ); // 3 bits to write reliability.
	//	bool hasSplitPacket = internalPacket->splitPacketCount>0; bitStream->Write(hasSplitPacket); // Write 1 bit to indicate if splitPacketCount>0
	bitLength = 8 * 1;

	//	bitStream->AlignWriteToByteBoundary();
	//	RakAssert(internalPacket->dataBitLength < 65535);
	//	unsigned short s; s = (unsigned short) internalPacket->dataBitLength; bitStream->WriteAlignedVar16((const char*)& s);
	bitLength += 8 * 2;

	if (internalPacket->reliability == MafiaNet::Reliability::Reliable ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
	    //		internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
		bitLength += 8 * 3; // bitStream->Write(internalPacket->reliableMessageNumber); // Message sequence number
	// bitStream->AlignWriteToByteBoundary(); // Potentially nothing else to write


	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced)
	{
		bitLength += 8 * 3;
		; // bitStream->Write(internalPacket->_sequencingIndex); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered.
	}

	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
	{
		bitLength += 8 * 3; // bitStream->Write(internalPacket->orderingIndex); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered.
		bitLength += 8 * 1; // tempChar=internalPacket->orderingChannel; bitStream->WriteAlignedVar8((const char*)& tempChar); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered. 5 bits needed, write one byte
	}
	if (internalPacket->splitPacketCount > 0)
	{
		bitLength += 8 * 4;                         // bitStream->WriteAlignedVar32((const char*)& internalPacket->splitPacketCount); RakAssert(sizeof(SplitPacketIndexType)==4); // Only needed if splitPacketCount>0. 4 bytes
		bitLength += 8 * sizeof(SplitPacketIdType); // bitStream->WriteAlignedVar16((const char*)& internalPacket->splitPacketId); RakAssert(sizeof(SplitPacketIdType)==2); // Only needed if splitPacketCount>0.
		bitLength += 8 * 4;                         // bitStream->WriteAlignedVar32((const char*)& internalPacket->splitPacketIndex); // Only needed if splitPacketCount>0. 4 bytes
	}

	return bitLength;
}

//-------------------------------------------------------------------------------------------------------
// Parse an internalPacket and create a bitstream to represent this data
//-------------------------------------------------------------------------------------------------------
BitSize_t ReliabilityLayer::WriteToBitStreamFromInternalPacket(MafiaNet::BitStream *bitStream, const InternalPacket *const internalPacket, CCTimeType curTime)
{
	(void) curTime;

	BitSize_t start = bitStream->GetNumberOfBitsUsed();
	unsigned char tempChar;

	// (Incoming data may be all zeros due to padding)
	bitStream->AlignWriteToByteBoundary(); // Potentially unaligned
	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableWithAckReceipt)
		tempChar = (unsigned char) MafiaNet::Reliability::Unreliable;
	else if (internalPacket->reliability == MafiaNet::Reliability::ReliableWithAckReceipt)
		tempChar = (unsigned char) MafiaNet::Reliability::Reliable;
	else if (internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
		tempChar = (unsigned char) MafiaNet::Reliability::ReliableOrdered;
	else
		tempChar = (unsigned char) internalPacket->reliability;

	bitStream->WriteBits((const unsigned char *) &tempChar, 3, true); // 3 bits to write reliability.

	bool hasSplitPacket = internalPacket->splitPacketCount > 0;
	bitStream->Write(hasSplitPacket); // Write 1 bit to indicate if splitPacketCount>0
	// 1 bit: the data starts with a re-split header (see ReSplitOversizedMessages)
	bitStream->Write(internalPacket->resplitCopy);
	bitStream->AlignWriteToByteBoundary();
	RakAssert(internalPacket->dataBitLength < 65535);
	unsigned short s;
	s = (unsigned short) internalPacket->dataBitLength;
	bitStream->WriteAlignedVar16((const char *) &s);
	if (internalPacket->reliability == MafiaNet::Reliability::Reliable ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
		bitStream->Write(internalPacket->reliableMessageNumber); // Used for all reliable types
	bitStream->AlignWriteToByteBoundary();                       // Potentially nothing else to write

	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced)
	{
		bitStream->Write(internalPacket->sequencingIndex); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered.
	}

	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
	{
		bitStream->Write(internalPacket->orderingIndex); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered.
		tempChar = internalPacket->orderingChannel;
		bitStream->WriteAlignedVar8((const char *) &tempChar); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered. 5 bits needed, write one byte
	}

	if (internalPacket->splitPacketCount > 0)
	{
		//	printf("Write before\n");
		//	bitStream->PrintBits();

		bitStream->WriteAlignedVar32((const char *) &internalPacket->splitPacketCount);
		RakAssert(sizeof(SplitPacketIndexType) == 4); // Only needed if splitPacketCount>0. 4 bytes
		bitStream->WriteAlignedVar16((const char *) &internalPacket->splitPacketId);
		RakAssert(sizeof(SplitPacketIdType) == 2);                                      // Only needed if splitPacketCount>0.
		bitStream->WriteAlignedVar32((const char *) &internalPacket->splitPacketIndex); // Only needed if splitPacketCount>0. 4 bytes

		//	printf("Write after\n");
		//	bitStream->PrintBits();
	}

	// Write the actual data.
	bitStream->WriteAlignedBytes((unsigned char *) internalPacket->data, BITS_TO_BYTES(internalPacket->dataBitLength));

	return bitStream->GetNumberOfBitsUsed() - start;
}

//-------------------------------------------------------------------------------------------------------
// Parse a bitstream and create an internal packet to represent this data
//-------------------------------------------------------------------------------------------------------
InternalPacket *ReliabilityLayer::CreateInternalPacketFromBitStream(MafiaNet::BitStream *bitStream, CCTimeType time)
{
	bool bitStreamSucceeded;
	InternalPacket *internalPacket;
	unsigned char tempChar;
	bool hasSplitPacket = false;
	bool readSuccess;

	if (bitStream->GetNumberOfUnreadBits() < (int) sizeof(internalPacket->reliableMessageNumber) * 8)
		return 0; // leftover bits

	internalPacket = AllocateFromInternalPacketPool();
	if (internalPacket == 0)
	{
		// Out of memory
		RakAssert(0);
		return 0;
	}
	internalPacket->creationTime = time;

	// (Incoming data may be all zeros due to padding)
	bitStream->AlignReadToByteBoundary(); // Potentially unaligned
	bitStream->ReadBits((unsigned char *) (&(tempChar)), 3);
	internalPacket->reliability = (MafiaNet::Reliability) tempChar;
	readSuccess = bitStream->Read(hasSplitPacket); // Read 1 bit to indicate if splitPacketCount>0
	readSuccess = bitStream->Read(internalPacket->resplitCopy) && readSuccess;
	bitStream->AlignReadToByteBoundary();
	unsigned short s;
	bitStream->ReadAlignedVar16((char *) &s);
	internalPacket->dataBitLength = s; // Length of message (2 bytes)
	if (internalPacket->reliability == MafiaNet::Reliability::Reliable ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered
	    // I don't write ACK_RECEIPT to the remote system
	    // 		||
	    // 		internalPacket->reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
	    // 		internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT ||
	    // 		internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt
	)
		bitStream->Read(internalPacket->reliableMessageNumber); // Message sequence number
	else
		internalPacket->reliableMessageNumber = (MessageNumberType) (uint32_t) -1;
	bitStream->AlignReadToByteBoundary(); // Potentially nothing else to Read

	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced)
	{
		bitStream->Read(internalPacket->sequencingIndex); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered.
	}

	if (internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrdered ||
	    internalPacket->reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt)
	{
		bitStream->Read(internalPacket->orderingIndex);                                      // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered. 4 bytes.
		readSuccess = bitStream->ReadAlignedVar8((char *) &internalPacket->orderingChannel); // Used for MafiaNet::Reliability::UnreliableSequenced, MafiaNet::Reliability::ReliableSequenced, MafiaNet::Reliability::ReliableOrdered. 5 bits needed, Read one byte
	}
	else
		internalPacket->orderingChannel = 0;

	if (hasSplitPacket)
	{
		// 		printf("Read before\n");
		// 		bitStream->PrintBits();

		bitStream->ReadAlignedVar32((char *) &internalPacket->splitPacketCount);               // Only needed if splitPacketCount>0. 4 bytes
		bitStream->ReadAlignedVar16((char *) &internalPacket->splitPacketId);                  // Only needed if splitPacketCount>0.
		readSuccess = bitStream->ReadAlignedVar32((char *) &internalPacket->splitPacketIndex); // Only needed if splitPacketCount>0. 4 bytes

		// 		printf("Read after\n");
		// 		bitStream->PrintBits();
	}
	else
	{
		internalPacket->splitPacketCount = 0;
	}

	if (readSuccess == false ||
	    internalPacket->dataBitLength == 0 ||
	    (unsigned int) internalPacket->reliability >= MafiaNet::NUMBER_OF_RELIABILITIES ||
	    internalPacket->orderingChannel >= 32 ||
	    (hasSplitPacket && (internalPacket->splitPacketIndex >= internalPacket->splitPacketCount)) ||
	    BITS_TO_BYTES(internalPacket->dataBitLength) >= MAXIMUM_MTU_SIZE)
	{
		// Malformed or hostile input from the remote: drop it. Not a RakAssert --
		// a Debug build must not abort on what a peer chooses to send.
		ReleaseToInternalPacketPool(internalPacket);
		return 0;
	}

	// Allocate memory to hold our data
	AllocInternalPacketData(internalPacket, BITS_TO_BYTES(internalPacket->dataBitLength), false);

	if (internalPacket->data == 0)
	{
		RakAssert("Out of memory in ReliabilityLayer::CreateInternalPacketFromBitStream" && 0);
		ReleaseToInternalPacketPool(internalPacket);
		return 0;
	}

	// Set the last byte to 0 so if ReadBits does not read a multiple of 8 the last bits are 0'ed out
	internalPacket->data[BITS_TO_BYTES(internalPacket->dataBitLength) - 1] = 0;

	// Read the data the packet holds
	bitStreamSucceeded = bitStream->ReadAlignedBytes((unsigned char *) internalPacket->data, BITS_TO_BYTES(internalPacket->dataBitLength));

	if (bitStreamSucceeded == false)
	{
		// The packet claims more data than the datagram carries: malformed or
		// hostile input, so drop it rather than RakAssert.
		FreeInternalPacketData(internalPacket);
		ReleaseToInternalPacketPool(internalPacket);
		return 0;
	}

	return internalPacket;
}


//-------------------------------------------------------------------------------------------------------
// Returns true if newPacketOrderingIndex is older than the waitingForPacketOrderingIndex
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsOlderOrderedPacket(OrderingIndexType newPacketOrderingIndex, OrderingIndexType waitingForPacketOrderingIndex)
{
	OrderingIndexType maxRange = (OrderingIndexType) (uint32_t) -1;

	if (waitingForPacketOrderingIndex > maxRange / (OrderingIndexType) 2)
	{
		if (newPacketOrderingIndex >= waitingForPacketOrderingIndex - maxRange / (OrderingIndexType) 2 + (OrderingIndexType) 1 && newPacketOrderingIndex < waitingForPacketOrderingIndex)
		{
			return true;
		}
	}
	else
	{
		if (newPacketOrderingIndex >= (OrderingIndexType) (waitingForPacketOrderingIndex - ((OrderingIndexType) maxRange / (OrderingIndexType) 2 + (OrderingIndexType) 1)) ||
		    newPacketOrderingIndex < waitingForPacketOrderingIndex)
		{
			return true;
		}
	}

	// Old packet
	return false;
}

//-------------------------------------------------------------------------------------------------------
// Split the passed packet into chunks under MTU_SIZEbytes (including headers) and save those new chunks
// Optimized version
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SplitPacket(InternalPacket *internalPacket)
{
	// Doing all sizes in bytes in this function so I don't write partial bytes with split packets
	internalPacket->splitPacketCount = 1; // This causes GetMessageHeaderLengthBits to account for the split packet header
	unsigned int headerLength = (unsigned int) BITS_TO_BYTES(GetMessageHeaderLengthBits(internalPacket));
	unsigned int dataByteLength = (unsigned int) BITS_TO_BYTES(internalPacket->dataBitLength);
	int maximumSendBlockBytes, byteOffset, bytesToSend;
	SplitPacketIndexType splitPacketIndex;
	int i;
	maximumSendBlockBytes = GetMaxDatagramSizeExcludingMessageHeaderBytes() - BITS_TO_BYTES(GetMaxMessageHeaderLengthBits());

	// Calculate how many packets we need to create
	internalPacket->splitPacketCount = ((dataByteLength - 1) / (maximumSendBlockBytes) + 1);

	std::vector<InternalPacket *> internalPacketArray(internalPacket->splitPacketCount);

	for (i = 0; i < (int) internalPacket->splitPacketCount; i++)
	{
		internalPacketArray[i] = AllocateFromInternalPacketPool();

		//		internalPacketArray[ i ] = sendPacketSet[internalPacket->priority].WriteLock();
		*internalPacketArray[i] = *internalPacket;
		internalPacketArray[i]->messageNumberAssigned = false;

		if (i != 0)
			internalPacket->messageInternalOrder = internalOrderIndex++;
	}

	// This identifies which packet this is in the set
	splitPacketIndex = 0;

	InternalPacketRefCountedData *refCounter = 0;

	// Do a loop to send out all the packets
	do
	{
		byteOffset = splitPacketIndex * maximumSendBlockBytes;
		bytesToSend = dataByteLength - byteOffset;

		if (bytesToSend > maximumSendBlockBytes)
			bytesToSend = maximumSendBlockBytes;

		// Copy over our chunk of data

		AllocInternalPacketData(internalPacketArray[splitPacketIndex], &refCounter, internalPacket->data, internalPacket->data + byteOffset);
		//		internalPacketArray[ splitPacketIndex ]->data = (unsigned char*) std::malloc(bytesToSend);
		//		memcpy( internalPacketArray[ splitPacketIndex ]->data, internalPacket->data + byteOffset, bytesToSend );

		if (bytesToSend != maximumSendBlockBytes)
			internalPacketArray[splitPacketIndex]->dataBitLength = internalPacket->dataBitLength - splitPacketIndex * (maximumSendBlockBytes << 3);
		else
			internalPacketArray[splitPacketIndex]->dataBitLength = bytesToSend << 3;

		internalPacketArray[splitPacketIndex]->splitPacketIndex = splitPacketIndex;
		internalPacketArray[splitPacketIndex]->splitPacketId = splitPacketId;
		internalPacketArray[splitPacketIndex]->splitPacketCount = internalPacket->splitPacketCount;
		internalPacketArray[splitPacketIndex]->splitOriginalBitLength = internalPacket->dataBitLength;
		RakAssert(internalPacketArray[splitPacketIndex]->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
	} while (++splitPacketIndex < internalPacket->splitPacketCount);

	splitPacketId++; // It's ok if this wraps to 0

	//	InternalPacket *workingPacket;

	// The fragments get non-decreasing weights, so each push_heap stops after
	// one comparison with its parent (what the old PushSeries shortcut did).
	RakAssert(outgoingPacketBuffer.empty() || outgoingPacketBuffer.front().packet->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));

	// Copy all the new packets into the split packet list
	for (i = 0; i < (int) internalPacket->splitPacketCount; i++)
	{
		internalPacketArray[i]->headerLength = headerLength;
		RakAssert(internalPacketArray[i]->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
		AddToUnreliableLinkedList(internalPacketArray[i]);
		//		sendPacketSet[ internalPacket->priority ].Push( internalPacketArray[ i ]);
		RakAssert(internalPacketArray[i]->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
		RakAssert(internalPacketArray[i]->messageNumberAssigned == false);
		const reliabilityHeapWeightType weight = GetNextWeight((int) internalPacketArray[i]->priority);
		HeapPush(outgoingPacketBuffer, outgoingPacketBufferNextSeq, weight, internalPacketArray[i]);
		RakAssert(outgoingPacketBuffer.empty() || outgoingPacketBuffer.front().packet->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
		statistics.messageInSendBuffer[(int) internalPacketArray[i]->priority]++;
		statistics.bytesInSendBuffer[(int) (int) internalPacketArray[i]->priority] += (double) BITS_TO_BYTES(internalPacketArray[i]->dataBitLength);
		//		workingPacket=sendPacketSet[internalPacket->priority].WriteLock();
		//		memcpy(workingPacket, internalPacketArray[ i ], sizeof(InternalPacket));
		//		sendPacketSet[internalPacket->priority].WriteUnlock();
	}

	// Do not delete, original is referenced by all split packets to avoid numerous allocations. See AllocInternalPacketData above
	//	FreeInternalPacketData(internalPacket);
	ReleaseToInternalPacketPool(internalPacket);
}

//-------------------------------------------------------------------------------------------------------
// In-session path-MTU black-hole recovery. Documented in reliability_layer.h.
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::StepDownMtuAfterBlackHole(void)
{
	const int nextMtu = NextLowerMtu(currentMtuBytes);
	if (nextMtu == 0)
		return;
	currentMtuBytes = nextMtu;
	mtuStepDowns++;
	// The failures counted so far were about the old size.
	mtuBlackHoleEvidence.Reset();
	// Same budget as Reset(): the wire MTU minus UDP/IP headers and the cipher overhead.
	congestionManager.SetMTU((uint32_t) (nextMtu - UDP_HEADER_SIZE - cipherOverheadBytes));
	ReSplitOversizedMessages();
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::ReSplitOversizedMessages(void)
{
	const BitSize_t maxDatagramBits = GetMaxDatagramSizeExcludingMessageHeaderBits();
	unsigned int i;
	InternalPacket *p;

	// One message to send again: its rebuilt payload, and what the receiver
	// must learn about the copy it replaces (see AddReSplitHeader).
	struct ReSplit
	{
		InternalPacket *rebuilt;
		bool wasSplit;
		SplitPacketIdType oldSplitId;
		// A fragment never left the outgoing buffer: the old copy cannot have
		// been delivered.
		bool unsentFragments;
		// Reliable message numbers of the old copy's unacknowledged packets.
		// They are never sent again, so the receiver is told about them.
		std::vector<MessageNumberType> retiredNumbers;
	};
	std::vector<ReSplit> reSplits;
	std::vector<InternalPacket *> resendVictims;

	// Rebuild a full-length copy of the message pkt belongs to, before any of
	// its packets are freed. Returns 0 (leaving the queues untouched) when the
	// rebuild is impossible, so a failure degrades to the old stalled-resend
	// behaviour rather than losing a reliable message.
	auto rebuildMessage = [this](InternalPacket *pkt) -> InternalPacket *
	{
		unsigned char *source;
		BitSize_t bitLength;
		if (pkt->splitPacketCount > 0)
		{
			// Fragments carry a slice of a shared, refcounted copy of the whole
			// message; splitOriginalBitLength is its exact full length.
			RakAssert(pkt->allocationScheme == InternalPacket::REF_COUNTED);
			RakAssert(pkt->refCountedData != 0 && pkt->splitOriginalBitLength > 0);
			if (pkt->allocationScheme != InternalPacket::REF_COUNTED || pkt->refCountedData == 0 || pkt->splitOriginalBitLength == 0)
				return 0;
			source = pkt->refCountedData->sharedDataBlock;
			bitLength = pkt->splitOriginalBitLength;
		}
		else
		{
			source = pkt->data;
			bitLength = pkt->dataBitLength;
		}
		const unsigned int byteLength = (unsigned int) BITS_TO_BYTES(bitLength);
		unsigned char *copy = (unsigned char *) std::malloc(byteLength);
		if (copy == 0)
		{
			return 0;
		}
		memcpy(copy, source, byteLength);
		InternalPacket *rebuilt = AllocateFromInternalPacketPool();
		if (rebuilt == 0)
		{
			std::free(copy);
			return 0;
		}
		// Keep reliability, priority, ordering/sequencing indices and receipt
		// serial: the receiver's ordered channel is waiting on exactly this
		// ordering index, so the re-split message slots into the stream where
		// the original stalled. resplitCopy is kept too: a copy's data already
		// starts with a re-split header, which AddReSplitHeader nests.
		*rebuilt = *pkt;
		AllocInternalPacketData(rebuilt, copy);
		rebuilt->refCountedData = 0;
		rebuilt->dataBitLength = bitLength;
		rebuilt->splitPacketCount = 0;
		rebuilt->splitPacketIndex = 0;
		rebuilt->splitPacketId = 0;
		rebuilt->splitOriginalBitLength = 0;
		rebuilt->messageNumberAssigned = false;
		rebuilt->timesSent = 0;
		rebuilt->nextActionTime = 0;
		rebuilt->retransmissionTime = 0;
		return rebuilt;
	};

	auto findStaleSplit = [&reSplits](SplitPacketIdType id) -> ReSplit *
	{
		for (ReSplit &r : reSplits)
			if (r.wasSplit && r.oldSplitId == id)
				return &r;
		return nullptr;
	};

	// An oversized fragment marks its whole message stale: every sibling is
	// pulled from the queues below and the message re-sent re-split under a
	// fresh splitPacketId.
	auto noteStaleSplit = [&](InternalPacket *pkt)
	{
		if (findStaleSplit(pkt->splitPacketId))
			return;
		InternalPacket *rebuilt = rebuildMessage(pkt);
		if (rebuilt == 0)
			return;
		reSplits.push_back(ReSplit{rebuilt, true, pkt->splitPacketId, false, {}});
	};

	// Drop a not-yet-sent packet in place. The outgoing buffer is a heap, so
	// entries cannot be unlinked by identity; the pop path already discards
	// data==0 tombstones (and settles the send-buffer statistics then).
	auto tombstoneOutgoing = [this](InternalPacket *pkt)
	{
		FreeInternalPacketData(pkt);
		pkt->data = 0;
		pkt->allocationScheme = InternalPacket::NORMAL;
		pkt->refCountedData = 0;
		RemoveFromUnreliableLinkedList(pkt);
	};

	// Pass 1: find every message with a packet that no longer fits a datagram.
	if (resendLinkedListHead)
	{
		p = resendLinkedListHead;
		do
		{
			if (GetMessageHeaderLengthBits(p) + p->dataBitLength > maxDatagramBits)
			{
				if (p->splitPacketCount > 0)
					noteStaleSplit(p);
				else
				{
					InternalPacket *rebuilt = rebuildMessage(p);
					if (rebuilt)
					{
						reSplits.push_back(ReSplit{rebuilt, false, 0, false, {p->reliableMessageNumber}});
						resendVictims.push_back(p);
					}
				}
			}
			p = p->resendNext;
		} while (p != resendLinkedListHead);
	}
	for (const HeapEntry &entry : outgoingPacketBuffer)
	{
		p = entry.packet;
		if (p->data == 0)
			continue;
		if (GetMessageHeaderLengthBits(p) + p->dataBitLength <= maxDatagramBits)
			continue;
		if (p->splitPacketCount > 0)
			noteStaleSplit(p);
		else
		{
			if (IsReliableForReSplit(p->reliability))
			{
				InternalPacket *rebuilt = rebuildMessage(p);
				if (rebuilt == 0)
					continue;
				// Never sent: no number to retire, nothing the receiver can hold.
				reSplits.push_back(ReSplit{rebuilt, false, 0, true, {}});
			}
			// An unreliable message the path already black-holed is not owed
			// delivery; dropping it here is what the network was doing anyway.
			tombstoneOutgoing(p);
		}
	}

	// Pass 2: sweep every packet of the stale messages out of both queues --
	// including small tail fragments that still fit, since the whole message
	// is re-sent under its new id -- noting the numbers retired with them.
	if (resendLinkedListHead)
	{
		p = resendLinkedListHead;
		do
		{
			ReSplit *r = p->splitPacketCount > 0 ? findStaleSplit(p->splitPacketId) : nullptr;
			if (r)
			{
				r->retiredNumbers.push_back(p->reliableMessageNumber);
				resendVictims.push_back(p);
			}
			p = p->resendNext;
		} while (p != resendLinkedListHead);
	}
	for (const HeapEntry &entry : outgoingPacketBuffer)
	{
		p = entry.packet;
		ReSplit *r = (p->data != 0 && p->splitPacketCount > 0) ? findStaleSplit(p->splitPacketId) : nullptr;
		if (r)
		{
			r->unsentFragments = true;
			tombstoneOutgoing(p);
		}
	}

	// Unlink the resend-list victims the way an ack would, minus the receipt.
	// Their numbers are never sent again: the re-split header retires them.
	for (i = 0; i < resendVictims.size(); i++)
	{
		p = resendVictims[i];
		if (resendBuffer[p->reliableMessageNumber & (uint32_t) RESEND_BUFFER_ARRAY_MASK] == p)
			resendBuffer[p->reliableMessageNumber & (uint32_t) RESEND_BUFFER_ARRAY_MASK] = 0;
		statistics.messagesInResendBuffer--;
		statistics.bytesInResendBuffer -= BITS_TO_BYTES(p->dataBitLength);
		RemoveFromList(p, true);
		FreeInternalPacketData(p);
		ReleaseToInternalPacketPool(p);
	}

	// Queue the rebuilt messages, split at the new, smaller size. A reliable
	// message the receiver may hold part or all of carries a re-split header.
	for (ReSplit &r : reSplits)
	{
		if (IsReliableForReSplit(r.rebuilt->reliability) && (r.wasSplit || !r.retiredNumbers.empty()))
			AddReSplitHeader(r.rebuilt, r.unsentFragments, r.wasSplit, r.oldSplitId, r.retiredNumbers);
		SplitPacket(r.rebuilt);
	}
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsReliableForReSplit(MafiaNet::Reliability reliability)
{
	return reliability == MafiaNet::Reliability::Reliable ||
	       reliability == MafiaNet::Reliability::ReliableOrdered ||
	       reliability == MafiaNet::Reliability::ReliableSequenced ||
	       reliability == MafiaNet::Reliability::ReliableWithAckReceipt ||
	       reliability == MafiaNet::Reliability::ReliableOrderedWithAckReceipt;
}

//-------------------------------------------------------------------------------------------------------
// The re-split header, at the start of a resplitCopy message's data (byte aligned):
//   u8  flags: RESPLIT_OLD_COPY_INCOMPLETE, RESPLIT_HAS_OLD_SPLIT_ID, RESPLIT_INNER_IS_COPY
//   u16 the old copy's splitPacketId, if RESPLIT_HAS_OLD_SPLIT_ID
//   u32 range count, then per range: u24 lowest, u24 highest reliable message number retired
// followed by the message (itself a re-split copy when RESPLIT_INNER_IS_COPY).
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AddReSplitHeader(InternalPacket *rebuilt, bool oldCopyIncomplete, bool wasSplit, SplitPacketIdType oldSplitId,
                                        std::vector<MessageNumberType> &retiredNumbers)
{
	std::sort(retiredNumbers.begin(), retiredNumbers.end(), [](const MessageNumberType &x, const MessageNumberType &y)
	          { return x.val < y.val; });
	std::vector<std::pair<MessageNumberType, MessageNumberType>> ranges;
	for (const MessageNumberType &number : retiredNumbers)
	{
		if (!ranges.empty() && ranges.back().second.val + 1 == number.val)
			ranges.back().second = number;
		else if (ranges.empty() || ranges.back().second.val != number.val)
			ranges.push_back(std::make_pair(number, number));
	}

	BitStream header;
	uint8_t flags = 0;
	if (oldCopyIncomplete)
		flags |= RESPLIT_OLD_COPY_INCOMPLETE;
	if (wasSplit)
		flags |= RESPLIT_HAS_OLD_SPLIT_ID;
	if (rebuilt->resplitCopy)
		flags |= RESPLIT_INNER_IS_COPY;
	header.Write(flags);
	if (wasSplit)
		header.Write(oldSplitId);
	header.Write((uint32_t) ranges.size());
	for (const auto &range : ranges)
	{
		header.Write(range.first);
		header.Write(range.second);
	}

	const unsigned int headerBytes = (unsigned int) header.GetNumberOfBytesUsed();
	const unsigned int payloadBytes = (unsigned int) BITS_TO_BYTES(rebuilt->dataBitLength);
	unsigned char *data = (unsigned char *) std::malloc(headerBytes + payloadBytes);
	if (data == 0)
	{
		// Sent without the header, the copy can be delivered twice or leave a
		// hole; out of memory, nothing better is on offer.
		return;
	}
	memcpy(data, header.GetData(), headerBytes);
	memcpy(data + headerBytes, rebuilt->data, payloadBytes);
	FreeInternalPacketData(rebuilt);
	AllocInternalPacketData(rebuilt, data);
	rebuilt->dataBitLength += BYTES_TO_BITS((BitSize_t) headerBytes);
	rebuilt->resplitCopy = true;
}

//-------------------------------------------------------------------------------------------------------
// Receiver side of a re-split copy. Documented in reliability_layer.h.
//-------------------------------------------------------------------------------------------------------
ReliabilityLayer::ReSplitVerdict ReliabilityLayer::ResolveReSplitCopy(InternalPacket *internalPacket)
{
	if (!IsReliableForReSplit(internalPacket->reliability))
		return RESPLIT_MALFORMED;

	// Every field comes from the remote, and an authenticated session is no proof of an honest one: the
	// work a header can cause is bounded by its message's size before anything is changed.
	//
	// How many numbers can a level honestly retire? The old copy it replaces is the data after the
	// level's header, and was sent split at the MTU in force before the step-down. A connection only
	// steps down from an MTU above the bottom rung, so every fragment but the last carried at least
	// minOldFragmentBytes: SplitPacket's block at that MTU with the largest overheads (cipher, datagram
	// header, message header). An old copy of n bytes therefore had at most n / minOldFragmentBytes + 1
	// packets, so at most that many unacked numbers to retire. At most RESPLIT_MAX_DEPTH levels nest,
	// one per step-down.
	const unsigned int minOldFragmentBytes = (unsigned int) (MTU_LADDER[MTU_LADDER_SIZE - 1] + 1 - UDP_HEADER_SIZE - DatagramCipher::kMaxOverhead) -
	                                         DatagramHeaderFormat::GetDataHeaderByteLength() - (unsigned int) BITS_TO_BYTES(GetMaxMessageHeaderLengthBits());

	struct Level
	{
		uint8_t flags;
		SplitPacketIdType oldSplitId;
		uint32_t rangeCount;
		BitSize_t rangesOffset;
		BitSize_t endOffset;
	};
	Level levels[RESPLIT_MAX_DEPTH];
	unsigned int depth = 0;
	const unsigned int byteLength = (unsigned int) BITS_TO_BYTES(internalPacket->dataBitLength);
	BitStream header(internalPacket->data, byteLength, false);

	// Pass 1: parse and check every level. Nothing is changed unless all of them are sound.
	for (;;)
	{
		if (depth == RESPLIT_MAX_DEPTH)
			return RESPLIT_MALFORMED;
		Level &level = levels[depth++];
		level.flags = 0;
		level.oldSplitId = 0;
		level.rangeCount = 0;
		if (!header.Read(level.flags) || (level.flags & ~RESPLIT_KNOWN_FLAGS) != 0)
			return RESPLIT_MALFORMED;
		if ((level.flags & RESPLIT_HAS_OLD_SPLIT_ID) != 0 && !header.Read(level.oldSplitId))
			return RESPLIT_MALFORMED;
		if (!header.Read(level.rangeCount))
			return RESPLIT_MALFORMED;
		level.rangesOffset = header.GetReadOffset();
		const unsigned int rangesStartByte = (unsigned int) BITS_TO_BYTES(level.rangesOffset);
		// The ranges and at least one byte of message must follow.
		if (rangesStartByte >= byteLength || level.rangeCount >= (byteLength - rangesStartByte) / RESPLIT_RANGE_BYTES + 1)
			return RESPLIT_MALFORMED;
		const unsigned int headerEndByte = rangesStartByte + level.rangeCount * RESPLIT_RANGE_BYTES;
		if (headerEndByte >= byteLength)
			return RESPLIT_MALFORMED;
		const uint32_t maxRetired = (byteLength - headerEndByte) / minOldFragmentBytes + 1;
		if (level.rangeCount > maxRetired)
			return RESPLIT_MALFORMED;
		uint32_t numbers = 0;
		for (uint32_t r = 0; r < level.rangeCount; r++)
		{
			MessageNumberType lowest((uint32_t) 0), highest((uint32_t) 0);
			if (!header.Read(lowest) || !header.Read(highest) || highest.val < lowest.val)
				return RESPLIT_MALFORMED;
			if (highest.val - lowest.val >= maxRetired - numbers)
				return RESPLIT_MALFORMED;
			numbers += highest.val - lowest.val + 1;
			// The old copy was numbered before this one: a number not older than the copy's own could
			// not have been retired, and closing it would pre-empt a message not sent yet.
			if (!IsOlderMessageNumber(lowest, internalPacket->reliableMessageNumber) || !IsOlderMessageNumber(highest, internalPacket->reliableMessageNumber) ||
			    !IsInReceiveWindow(lowest) || !IsInReceiveWindow(highest))
				return RESPLIT_MALFORMED;
		}
		level.endOffset = header.GetReadOffset();
		if ((level.flags & RESPLIT_INNER_IS_COPY) == 0)
			break;
		// A copy of a copy: the inner header, about the generation before, follows.
	}

	// Pass 2: resolve the levels outermost first. Close every retired number. One the receiver never
	// had proves the old copy can no longer complete -- that packet will now be dropped as a duplicate
	// -- so this copy stands, and the next level decides about the generation before. If it had them
	// all, the old copy arrived whole and was handled then: this one is a duplicate.
	for (unsigned int l = 0; l < depth; l++)
	{
		bool deliver = (levels[l].flags & RESPLIT_OLD_COPY_INCOMPLETE) != 0;
		header.SetReadOffset(levels[l].rangesOffset);
		for (uint32_t r = 0; r < levels[l].rangeCount; r++)
		{
			MessageNumberType lowest((uint32_t) 0), highest((uint32_t) 0);
			header.Read(lowest);
			header.Read(highest);
			for (MessageNumberType number = lowest;; ++number)
			{
				reSplitNumbersMarked++;
				if (MarkReliableMessageNumberReceived(number))
					deliver = true;
				if (number == highest)
					break;
			}
		}
		if (!deliver)
			return RESPLIT_DISCARD;
		// Whatever arrived of the old copy will never be completed.
		if ((levels[l].flags & RESPLIT_HAS_OLD_SPLIT_ID) != 0)
			DiscardSplitPacketChannel(levels[l].oldSplitId);
	}

	const BitSize_t headerBits = levels[depth - 1].endOffset;
	memmove(internalPacket->data, internalPacket->data + BITS_TO_BYTES(headerBits), (size_t) BITS_TO_BYTES(internalPacket->dataBitLength - headerBits));
	internalPacket->dataBitLength -= headerBits;
	internalPacket->resplitCopy = false;
	return RESPLIT_DELIVER;
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsInReceiveWindow(MessageNumberType number) const
{
	// The same bounds HandleSocketReceiveFromConnectedPlayer applies to a reliable message.
	const DatagramSequenceNumberType holeCount = (DatagramSequenceNumberType) (number - receivedPacketsBaseIndex);
	const DatagramSequenceNumberType typeRange = (DatagramSequenceNumberType) (uint32_t) -1;
	if (holeCount > typeRange / (DatagramSequenceNumberType) 2)
		return true; // Older than the window: already received.
	return (unsigned int) holeCount < hasReceivedPacketQueue.size() || holeCount <= (DatagramSequenceNumberType) maxReceiveWindow;
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::MarkReliableMessageNumberReceived(MessageNumberType number)
{
	// The bookkeeping HandleSocketReceiveFromConnectedPlayer does for an arriving reliable message.
	const DatagramSequenceNumberType holeCount = (DatagramSequenceNumberType) (number - receivedPacketsBaseIndex);
	const DatagramSequenceNumberType typeRange = (DatagramSequenceNumberType) (uint32_t) -1;
	if (holeCount > typeRange / (DatagramSequenceNumberType) 2)
		return false; // Older than the window: already received.
	if ((unsigned int) holeCount < hasReceivedPacketQueue.size())
	{
		if (hasReceivedPacketQueue[holeCount] == false)
			return false; // Already received.
		hasReceivedPacketQueue[holeCount] = false;
	}
	else
	{
		while ((unsigned int) holeCount > hasReceivedPacketQueue.size())
			hasReceivedPacketQueue.push_back(true);
		hasReceivedPacketQueue.push_back(false);
	}
	while (!hasReceivedPacketQueue.empty() && !hasReceivedPacketQueue.front())
	{
		hasReceivedPacketQueue.pop_front();
		++receivedPacketsBaseIndex;
	}
	return true;
}

//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::DiscardSplitPacketChannel(SplitPacketIdType id)
{
	const auto channelIt = splitPacketChannelList.find(id);
	if (channelIt == splitPacketChannelList.end())
		return;
	SplitPacketChannel *channel = channelIt->second;
	for (const auto &fragment : channel->splitPacketList.Fragments())
	{
		FreeInternalPacketData(fragment.second);
		ReleaseToInternalPacketPool(fragment.second);
	}
#if PREALLOCATE_LARGE_MESSAGES == 1
	if (channel->returnedPacket)
	{
		FreeInternalPacketData(channel->returnedPacket);
		ReleaseToInternalPacketPool(channel->returnedPacket);
	}
#endif
	delete channel;
	splitPacketChannelList.erase(channelIt);
}

//-------------------------------------------------------------------------------------------------------
// Insert a packet into the split packet list
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::InsertIntoSplitPacketList(InternalPacket *internalPacket, CCTimeType time)
{
	// Find the SplitPacketChannel for this splitPacketId. If this is the first fragment seen, allocate one and insert it.
	auto channelIt = splitPacketChannelList.find(internalPacket->splitPacketId);
	if (channelIt == splitPacketChannelList.end())
	{
		SplitPacketChannel *newChannel = new SplitPacketChannel;
		channelIt = splitPacketChannelList.emplace(internalPacket->splitPacketId, newChannel).first;
#if PREALLOCATE_LARGE_MESSAGES == 1
		newChannel->returnedPacket = CreateInternalPacketCopy(internalPacket, 0, 0, time);
		newChannel->gotFirstPacket = false;
		newChannel->splitPacketsArrived = 0;
		AllocInternalPacketData(newChannel->returnedPacket, BITS_TO_BYTES(internalPacket->dataBitLength * internalPacket->splitPacketCount), false);
		RakAssert(newChannel->returnedPacket->data);
#else
		newChannel->firstPacket = 0;
		newChannel->splitPacketList.Init(internalPacket);
#endif
	}
	SplitPacketChannel *channel = channelIt->second;

#if PREALLOCATE_LARGE_MESSAGES == 1
	channel->lastUpdateTime = time;
	channel->splitPacketsArrived++;
	channel->returnedPacket->dataBitLength += internalPacket->dataBitLength;

	bool dealloc;
	if (internalPacket->splitPacketIndex == 0)
	{
		channel->gotFirstPacket = true;
		channel->stride = BITS_TO_BYTES(internalPacket->dataBitLength);

		for (unsigned int j = 0; j < channel->splitPacketList.Size(); j++)
		{
			memcpy(channel->returnedPacket->data + internalPacket->splitPacketIndex * channel->stride, internalPacket->data, (size_t) BITS_TO_BYTES(internalPacket->dataBitLength));
			FreeInternalPacketData(channel->splitPacketList[j]);
			ReleaseToInternalPacketPool(channel->splitPacketList[j]);
		}

		memcpy(channel->returnedPacket->data, internalPacket->data, (size_t) BITS_TO_BYTES(internalPacket->dataBitLength));
		channel->splitPacketList.Clear(true);
		dealloc = true;
	}
	else
	{
		if (channel->gotFirstPacket == true)
		{
			memcpy(channel->returnedPacket->data + internalPacket->splitPacketIndex * channel->stride, internalPacket->data, (size_t) BITS_TO_BYTES(internalPacket->dataBitLength));
			dealloc = true;
		}
		else
		{
			channel->splitPacketList.Push(internalPacket);
			dealloc = false;
		}
	}

	if (channel->gotFirstPacket == true &&
	    splitMessageProgressInterval &&
	    // 		channel->firstPacket &&
	    // 		channel->splitPacketList.Size()!=channel->firstPacket->splitPacketCount &&
	    // 		(channel->splitPacketList.Size()%splitMessageProgressInterval)==0
	    channel->gotFirstPacket &&
	    channel->splitPacketsArrived != channel->returnedPacket->splitPacketCount &&
	    (channel->splitPacketsArrived % splitMessageProgressInterval) == 0)
	{
		// Return ID_DOWNLOAD_PROGRESS
		// Write splitPacketIndex (SplitPacketIndexType)
		// Write splitPacketCount (SplitPacketIndexType)
		// Write byteLength (4)
		// Write data, the first fragment's data
		InternalPacket *progressIndicator = AllocateFromInternalPacketPool();
		//		unsigned int len = sizeof(MessageID) + sizeof(unsigned int)*2 + sizeof(unsigned int) + (unsigned int) BITS_TO_BYTES(channel->firstPacket->dataBitLength);
		unsigned int l = (unsigned int) channel->stride;
		const unsigned int len = sizeof(MessageID) + sizeof(unsigned int) * 2 + sizeof(unsigned int) + l;
		AllocInternalPacketData(progressIndicator, len, false);
		progressIndicator->dataBitLength = BYTES_TO_BITS(len);
		progressIndicator->data[0] = (MessageID) ID_DOWNLOAD_PROGRESS;
		unsigned int temp;
		//	temp=channel->splitPacketList.Size();
		temp = channel->splitPacketsArrived;
		memcpy(progressIndicator->data + sizeof(MessageID), &temp, sizeof(unsigned int));
		temp = (unsigned int) internalPacket->splitPacketCount;
		memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 1, &temp, sizeof(unsigned int));
		//		temp=(unsigned int) BITS_TO_BYTES(channel->firstPacket->dataBitLength);
		temp = (unsigned int) BITS_TO_BYTES(l);
		memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 2, &temp, sizeof(unsigned int));
		//memcpy(progressIndicator->data+sizeof(MessageID)+sizeof(unsigned int)*3, channel->firstPacket->data, (size_t) BITS_TO_BYTES(channel->firstPacket->dataBitLength));
		memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 3, channel->returnedPacket->data, (size_t) BITS_TO_BYTES(l));
	}

	if (dealloc)
	{
		FreeInternalPacketData(internalPacket);
		ReleaseToInternalPacketPool(internalPacket);
	}
#else
	// Insert the packet into the SplitPacketChannel
	if (!channel->splitPacketList.Add(internalPacket))
	{
		FreeInternalPacketData(internalPacket);
		ReleaseToInternalPacketPool(internalPacket);
		// A channel whose first fragment was rejected holds nothing; drop it so
		// a malformed fragment cannot pin a channel slot.
		if (channel->splitPacketList.GetNumAddedPackets() == 0)
		{
			delete channel;
			splitPacketChannelList.erase(channelIt);
		}
		return;
	}

	channel->lastUpdateTime = time;

	// If the index is 0, then this is the first packet. Record this so it can be returned to the user with download progress
	if (internalPacket->splitPacketIndex == 0)
		channel->firstPacket = internalPacket;

	// Return download progress if we have the first packet, the list is not complete, and there are enough packets to justify it
	if (splitMessageProgressInterval &&
	    channel->firstPacket &&
	    channel->splitPacketList.GetNumAddedPackets() != channel->firstPacket->splitPacketCount &&
	    (channel->splitPacketList.GetNumAddedPackets() % splitMessageProgressInterval) == 0)
	{
		// Return ID_DOWNLOAD_PROGRESS
		// Write splitPacketIndex (SplitPacketIndexType)
		// Write splitPacketCount (SplitPacketIndexType)
		// Write byteLength (4)
		// Write data, the first fragment's data
		InternalPacket *progressIndicator = AllocateFromInternalPacketPool();
		unsigned int length = sizeof(MessageID) + sizeof(unsigned int) * 2 + sizeof(unsigned int) + (unsigned int) BITS_TO_BYTES(channel->firstPacket->dataBitLength);
		AllocInternalPacketData(progressIndicator, length, false);
		progressIndicator->dataBitLength = BYTES_TO_BITS(length);
		progressIndicator->data[0] = (MessageID) ID_DOWNLOAD_PROGRESS;
		unsigned int temp;
		temp = channel->splitPacketList.GetNumAddedPackets();
		memcpy(progressIndicator->data + sizeof(MessageID), &temp, sizeof(unsigned int));
		temp = (unsigned int) internalPacket->splitPacketCount;
		memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 1, &temp, sizeof(unsigned int));
		temp = (unsigned int) BITS_TO_BYTES(channel->firstPacket->dataBitLength);
		memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 2, &temp, sizeof(unsigned int));

		memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 3, channel->firstPacket->data, (size_t) BITS_TO_BYTES(channel->firstPacket->dataBitLength));
		outputQueue.push_back(progressIndicator);
	}

#endif
}

//-------------------------------------------------------------------------------------------------------
// Take all split chunks with the specified splitPacketId and try to
//reconstruct a packet.  If we can, allocate and return it.  Otherwise return 0
// Optimized version
//-------------------------------------------------------------------------------------------------------
InternalPacket *ReliabilityLayer::BuildPacketFromSplitPacketList(SplitPacketChannel *splitPacketChannel, CCTimeType time)
{
#if PREALLOCATE_LARGE_MESSAGES == 1
	InternalPacket *returnedPacket = splitPacketChannel->returnedPacket;
	delete splitPacketChannel;
	(void) time;
	return returnedPacket;
#else
	const std::map<SplitPacketIndexType, InternalPacket *> &fragments = splitPacketChannel->splitPacketList.Fragments();

	// The fragment lengths come from the remote: sum them wide and refuse a
	// message whose length does not fit BitSize_t rather than wrap around and
	// allocate too little.
	uint64_t totalBits = 0;
	for (const auto &fragment : fragments)
		totalBits += fragment.second->dataBitLength;

	InternalPacket *internalPacket = nullptr;
	if (totalBits <= (uint64_t) (BitSize_t) -1)
	{
		internalPacket = CreateInternalPacketCopy(fragments.begin()->second, 0, 0, time);
		internalPacket->dataBitLength = (BitSize_t) totalBits;
		internalPacket->data = (unsigned char *) std::malloc((size_t) BITS_TO_BYTES(internalPacket->dataBitLength));
		internalPacket->allocationScheme = InternalPacket::NORMAL;

		// Add() guarantees every fragment but the last is whole bytes, so the
		// byte offsets below are exact and the copies end inside the buffer.
		BitSize_t offset = 0;
		for (const auto &fragment : fragments)
		{
			InternalPacket *splitPacket = fragment.second;
			memcpy(internalPacket->data + BITS_TO_BYTES(offset), splitPacket->data, (size_t) BITS_TO_BYTES(splitPacket->dataBitLength));
			offset += splitPacket->dataBitLength;
		}
	}

	for (const auto &fragment : fragments)
	{
		FreeInternalPacketData(fragment.second);
		ReleaseToInternalPacketPool(fragment.second);
	}
	delete splitPacketChannel;

	return internalPacket;
#endif
}
//-------------------------------------------------------------------------------------------------------
InternalPacket *ReliabilityLayer::BuildPacketFromSplitPacketList(SplitPacketIdType inSplitPacketId, CCTimeType time,
                                                                 RakNetSocket2 *s, SystemAddress &systemAddress, RakNetRandom *rnr,
                                                                 BitStream &updateBitStream)
{
	SplitPacketChannel *splitPacketChannel;
	InternalPacket *internalPacket;

	// Find in splitPacketChannelList the SplitPacketChannel with this splitPacketId.
	// It is gone if the fragment just inserted was rejected and was its only one.
	const auto channelIt = splitPacketChannelList.find(inSplitPacketId);
	if (channelIt == splitPacketChannelList.end())
		return 0;
	splitPacketChannel = channelIt->second;

#if PREALLOCATE_LARGE_MESSAGES == 1
	if (splitPacketChannel->splitPacketsArrived == splitPacketChannel->returnedPacket->splitPacketCount)
#else
	if (splitPacketChannel->splitPacketList.AllPacketsAdded())
#endif
	{
		// Ack immediately, because for large files this can take a long time
		SendACKs(s, systemAddress, time, rnr, updateBitStream);
		internalPacket = BuildPacketFromSplitPacketList(splitPacketChannel, time); // deletes the channel
		splitPacketChannelList.erase(channelIt);
		return internalPacket;
	}
	else
	{
		return 0;
	}
}

//-------------------------------------------------------------------------------------------------------
// Creates a copy of the specified internal packet with data copied from the original starting at dataByteOffset for dataByteLength bytes.
// Does not copy any split data parameters as that information is always generated does not have any reason to be copied
//-------------------------------------------------------------------------------------------------------
InternalPacket *ReliabilityLayer::CreateInternalPacketCopy(InternalPacket *original, int dataByteOffset, int dataByteLength, CCTimeType time)
{
	InternalPacket *copy = AllocateFromInternalPacketPool();
#ifdef _DEBUG
	// Remove accessing undefined memory error: deliberately poison every byte so a field this
	// function forgets to set shows up as 0xFF. The only non-trivial members are uint24_t
	// wrappers around a uint32_t, so a byte fill is sound; the void* cast says so to the compiler.
	memset(static_cast<void *>(copy), 255, sizeof(InternalPacket));
#endif
	// Copy over our chunk of data

	if (dataByteLength > 0)
	{
		AllocInternalPacketData(copy, BITS_TO_BYTES(dataByteLength), false);
		memcpy(copy->data, original->data + dataByteOffset, dataByteLength);
	}
	else
		copy->data = 0;

	copy->dataBitLength = dataByteLength << 3;
	copy->creationTime = time;
	copy->nextActionTime = 0;
	copy->orderingIndex = original->orderingIndex;
	copy->sequencingIndex = original->sequencingIndex;
	copy->orderingChannel = original->orderingChannel;
	copy->reliableMessageNumber = original->reliableMessageNumber;
	copy->priority = original->priority;
	copy->reliability = original->reliability;
	copy->resplitCopy = original->resplitCopy;
#if PREALLOCATE_LARGE_MESSAGES == 1
	copy->splitPacketCount = original->splitPacketCount;
	copy->splitPacketId = original->splitPacketId;
	copy->splitPacketIndex = original->splitPacketIndex;
#endif

	return copy;
}


//-------------------------------------------------------------------------------------------------------
// Inserts a packet into the resend list in order
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::InsertPacketIntoResendList(InternalPacket *internalPacket, CCTimeType time, bool firstResend, bool modifyUnacknowledgedBytes)
{
	(void) firstResend;
	(void) time;
	(void) internalPacket;

	AddToListTail(internalPacket, modifyUnacknowledgedBytes);
	RakAssert(internalPacket->nextActionTime != 0);
}

//-------------------------------------------------------------------------------------------------------
//  Were you ever unable to deliver a packet despite retries?
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsDeadConnection(void) const
{
	return deadConnection;
}

//-------------------------------------------------------------------------------------------------------
//  Causes IsDeadConnection to return true
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::KillConnection(void)
{
	deadConnection = true;
}


//-------------------------------------------------------------------------------------------------------
// Statistics
//-------------------------------------------------------------------------------------------------------
RakNetStatistics *ReliabilityLayer::GetStatistics(RakNetStatistics *rns)
{
	unsigned i;
	MafiaNet::TimeUS time = MafiaNet::GetTimeUS();
	uint64_t uint64Denominator;
	double doubleDenominator;

	for (i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++)
	{
		statistics.valueOverLastSecond[i] = bpsMetrics[i].GetBPS1Threadsafe(time);
		statistics.runningTotal[i] = bpsMetrics[i].GetTotal1();
	}

	memcpy(rns, &statistics, sizeof(statistics));

	if (rns->valueOverLastSecond[USER_MESSAGE_BYTES_SENT] + rns->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT] > 0)
		rns->packetlossLastSecond = (float) ((double) rns->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT] / ((double) rns->valueOverLastSecond[USER_MESSAGE_BYTES_SENT] + (double) rns->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT]));
	else
		rns->packetlossLastSecond = 0.0f;

	rns->packetlossTotal = 0.0f;
	uint64Denominator = (rns->runningTotal[USER_MESSAGE_BYTES_SENT] + rns->runningTotal[USER_MESSAGE_BYTES_RESENT]);
	if (uint64Denominator != 0 && rns->runningTotal[USER_MESSAGE_BYTES_SENT] / uint64Denominator > 0)
	{
		doubleDenominator = ((double) rns->runningTotal[USER_MESSAGE_BYTES_SENT] + (double) rns->runningTotal[USER_MESSAGE_BYTES_RESENT]);
		if (doubleDenominator != 0)
		{
			rns->packetlossTotal = (float) ((double) rns->runningTotal[USER_MESSAGE_BYTES_RESENT] / doubleDenominator);
		}
	}

	rns->droppedUnauthenticatedDatagrams = droppedUnauthenticatedDatagrams;
	rns->mtuStepDowns = mtuStepDowns;
	rns->isLimitedByCongestionControl = statistics.isLimitedByCongestionControl;
	rns->BPSLimitByCongestionControl = statistics.BPSLimitByCongestionControl;
	rns->isLimitedByOutgoingBandwidthLimit = statistics.isLimitedByOutgoingBandwidthLimit;
	rns->BPSLimitByOutgoingBandwidthLimit = statistics.BPSLimitByOutgoingBandwidthLimit;

	return rns;
}

//-------------------------------------------------------------------------------------------------------
// Returns the number of packets in the resend queue, not counting holes
//-------------------------------------------------------------------------------------------------------
unsigned int ReliabilityLayer::GetResendListDataSize(void) const
{
	// Not accurate but thread-safe.  The commented version might crash if the queue is cleared while we loop through it
	// return resendTree.Size();
	return statistics.messagesInResendBuffer;
}

//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::AckTimeout(MafiaNet::Time curTime)
{
	// I check timeLastDatagramArrived-curTime because with threading it is possible that timeLastDatagramArrived is
	// slightly greater than curTime, in which case this is NOT an ack timeout
	return (timeLastDatagramArrived - curTime) > 10000 && curTime - timeLastDatagramArrived > timeoutTime;
}
//-------------------------------------------------------------------------------------------------------
CCTimeType ReliabilityLayer::GetNextSendTime(void) const
{
	return nextSendTime;
}
//-------------------------------------------------------------------------------------------------------
CCTimeType ReliabilityLayer::GetTimeBetweenPackets(void) const
{
	return timeBetweenPackets;
}
//-------------------------------------------------------------------------------------------------------
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
CCTimeType ReliabilityLayer::GetAckPing(void) const
{
	return ackPing;
}
#endif
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::ResetPacketsAndDatagrams(void)
{
	packetsToSendThisUpdate.clear();
	packetsToDeallocThisUpdate.clear();
	packetsToSendThisUpdateDatagramBoundaries.clear();
	datagramsToSendThisUpdateIsPair.clear();
	datagramSizesInBytes.clear();
	datagramSizeSoFar = 0;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::PushPacket(CCTimeType time, InternalPacket *internalPacket, bool isReliable)
{
	BitSize_t bitsForThisPacket = BYTES_TO_BITS(BITS_TO_BYTES(internalPacket->dataBitLength) + BITS_TO_BYTES(internalPacket->headerLength));
	datagramSizeSoFar += bitsForThisPacket;
	RakAssert(BITS_TO_BYTES(datagramSizeSoFar) < MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
	allDatagramSizesSoFar += bitsForThisPacket;
	internalPacket->lastTransmissionTime = time;
	packetsToSendThisUpdate.push_back(internalPacket);
	packetsToDeallocThisUpdate.push_back(isReliable == false);
	RakAssert(internalPacket->headerLength == GetMessageHeaderLengthBits(internalPacket));

	// This code tells me how much time elapses between when you send, and when the message actually goes out
	// 	if (internalPacket->data[0]==0)
	// 	{
	// 		MafiaNet::TimeMS t;
	// 		MafiaNet::BitStream bs(internalPacket->data+1,sizeof(t),false);
	// 		bs.Read(t);
	// 		MafiaNet::TimeMS curTime=MafiaNet::GetTimeMS();
	// 		MafiaNet::TimeMS diff = curTime-t;
	// 	}

	congestionManager.OnSendBytes(time, BITS_TO_BYTES(internalPacket->dataBitLength) + BITS_TO_BYTES(internalPacket->headerLength));
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::PushDatagram(void)
{
	if (datagramSizeSoFar > 0)
	{
		packetsToSendThisUpdateDatagramBoundaries.push_back((unsigned int) packetsToSendThisUpdate.size());
		datagramsToSendThisUpdateIsPair.push_back(false);
		RakAssert(BITS_TO_BYTES(datagramSizeSoFar) < MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
		datagramSizesInBytes.push_back(BITS_TO_BYTES(datagramSizeSoFar));
		datagramSizeSoFar = 0;

		// Disable packet pairs
		/*
		if (countdownToNextPacketPair==0)
		{
		if (TagMostRecentPushAsSecondOfPacketPair())
		countdownToNextPacketPair=15;
		}
		else
		countdownToNextPacketPair--;
		*/
	}
}
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::TagMostRecentPushAsSecondOfPacketPair(void)
{
	if (datagramsToSendThisUpdateIsPair.size() >= 2)
	{
		datagramsToSendThisUpdateIsPair[datagramsToSendThisUpdateIsPair.size() - 2] = true;
		datagramsToSendThisUpdateIsPair[datagramsToSendThisUpdateIsPair.size() - 1] = true;
		return true;
	}
	return false;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::ClearPacketsAndDatagrams(void)
{
	unsigned int i;
	for (i = 0; i < packetsToDeallocThisUpdate.size(); i++)
	{
		// packetsToDeallocThisUpdate holds a boolean indicating if packetsToSendThisUpdate at this index should be freed
		if (packetsToDeallocThisUpdate[i])
		{
			RemoveFromUnreliableLinkedList(packetsToSendThisUpdate[i]);
			FreeInternalPacketData(packetsToSendThisUpdate[i]);
			// if (keepInternalPacketIfNeedsAck==false || packetsToSendThisUpdate[i]->reliability<MafiaNet::Reliability::ReliableWithAckReceipt)
			ReleaseToInternalPacketPool(packetsToSendThisUpdate[i]);
		}
	}
	packetsToDeallocThisUpdate.clear();
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::MoveToListHead(InternalPacket *internalPacket)
{
	if (internalPacket == resendLinkedListHead)
		return;
	if (resendLinkedListHead == 0)
	{
		internalPacket->resendNext = internalPacket;
		internalPacket->resendPrev = internalPacket;
		resendLinkedListHead = internalPacket;
		return;
	}
	internalPacket->resendPrev->resendNext = internalPacket->resendNext;
	internalPacket->resendNext->resendPrev = internalPacket->resendPrev;
	internalPacket->resendNext = resendLinkedListHead;
	internalPacket->resendPrev = resendLinkedListHead->resendPrev;
	internalPacket->resendPrev->resendNext = internalPacket;
	resendLinkedListHead->resendPrev = internalPacket;
	resendLinkedListHead = internalPacket;
	RakAssert(internalPacket->headerLength + internalPacket->dataBitLength > 0);

	//ValidateResendList();
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::RemoveFromList(InternalPacket *internalPacket, bool modifyUnacknowledgedBytes)
{
	InternalPacket *newPosition;
	internalPacket->resendPrev->resendNext = internalPacket->resendNext;
	internalPacket->resendNext->resendPrev = internalPacket->resendPrev;
	newPosition = internalPacket->resendNext;
	if (internalPacket == resendLinkedListHead)
		resendLinkedListHead = newPosition;
	if (resendLinkedListHead == internalPacket)
		resendLinkedListHead = 0;

	if (modifyUnacknowledgedBytes)
	{
		RakAssert(unacknowledgedBytes >= BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength));
		unacknowledgedBytes -= BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength);
		// printf("-unacknowledgedBytes:%i ", unacknowledgedBytes);


		//		ValidateResendList();
	}
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AddToListTail(InternalPacket *internalPacket, bool modifyUnacknowledgedBytes)
{
	if (modifyUnacknowledgedBytes)
	{
		unacknowledgedBytes += BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength);
		// printf("+unacknowledgedBytes:%i ", unacknowledgedBytes);
	}

	if (resendLinkedListHead == 0)
	{
		internalPacket->resendNext = internalPacket;
		internalPacket->resendPrev = internalPacket;
		resendLinkedListHead = internalPacket;
		return;
	}
	internalPacket->resendNext = resendLinkedListHead;
	internalPacket->resendPrev = resendLinkedListHead->resendPrev;
	internalPacket->resendPrev->resendNext = internalPacket;
	resendLinkedListHead->resendPrev = internalPacket;

	//	ValidateResendList();
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::PopListHead(bool modifyUnacknowledgedBytes)
{
	RakAssert(resendLinkedListHead != 0);
	RemoveFromList(resendLinkedListHead, modifyUnacknowledgedBytes);
}
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::IsResendQueueEmpty(void) const
{
	return resendLinkedListHead == 0;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::SendACKs(RakNetSocket2 *s, SystemAddress &systemAddress, CCTimeType time, RakNetRandom *rnr, BitStream &updateBitStream)
{
	BitSize_t maxDatagramPayload = GetMaxDatagramSizeExcludingMessageHeaderBits();

	while (acknowlegements.Size() > 0)
	{
		// Send acks
		updateBitStream.Reset();
		DatagramHeaderFormat dhf;
		dhf.isACK = true;
		dhf.isNAK = false;
		dhf.isPacketPair = false;
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
		dhf.sourceSystemTime = time;
#endif
		double B;
		double AS;
		bool hasBAndAS;
		if (remoteSystemNeedsBAndAS)
		{
			congestionManager.OnSendAckGetBAndAS(time, &hasBAndAS, &B, &AS);
			dhf.AS = (float) AS;
			dhf.hasBAndAS = hasBAndAS;
		}
		else
			dhf.hasBAndAS = false;
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
		dhf.sourceSystemTime = nextAckTimeToSend;
#endif
		//		dhf.B=(float)B;
		updateBitStream.Reset();
		dhf.Serialize(&updateBitStream);
		CC_DEBUG_PRINTF_1("AckSnd ");
		acknowlegements.Serialize(&updateBitStream, maxDatagramPayload, true);
		SendBitStream(s, systemAddress, &updateBitStream, rnr, time);
		congestionManager.OnSendAck(time, updateBitStream.GetNumberOfBytesUsed());

		// I think this is causing a bug where if the estimated bandwidth is very low for the recipient, only acks ever get sent
		//	congestionManager.OnSendBytes(time,UDP_HEADER_SIZE+updateBitStream.GetNumberOfBytesUsed());
	}
}
//-------------------------------------------------------------------------------------------------------
InternalPacket *ReliabilityLayer::AllocateFromInternalPacketPool(void)
{
	InternalPacket *ip = internalPacketPool.Acquire();
	ip->reliableMessageNumber = (MessageNumberType) (uint32_t) -1;
	ip->messageNumberAssigned = false;
	ip->nextActionTime = 0;
	ip->splitPacketCount = 0;
	ip->splitPacketIndex = 0;
	ip->splitPacketId = 0;
	ip->allocationScheme = InternalPacket::NORMAL;
	ip->data = 0;
	ip->timesSent = 0;
	ip->splitOriginalBitLength = 0;
	ip->resplitCopy = false;
	ip->lastTransmissionTime = 0;
	return ip;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::ReleaseToInternalPacketPool(InternalPacket *ip)
{
	internalPacketPool.Release(ip);
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::RemoveFromUnreliableLinkedList(InternalPacket *internalPacket)
{
	if (internalPacket->reliability == MafiaNet::Reliability::Unreliable ||
	    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::UnreliableWithAckReceipt
	    //		||
	    //		internalPacket->reliability==UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
	)
	{
		InternalPacket *newPosition;
		internalPacket->unreliablePrev->unreliableNext = internalPacket->unreliableNext;
		internalPacket->unreliableNext->unreliablePrev = internalPacket->unreliablePrev;
		newPosition = internalPacket->unreliableNext;
		if (internalPacket == unreliableLinkedListHead)
			unreliableLinkedListHead = newPosition;
		if (unreliableLinkedListHead == internalPacket)
			unreliableLinkedListHead = 0;
	}
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AddToUnreliableLinkedList(InternalPacket *internalPacket)
{
	if (internalPacket->reliability == MafiaNet::Reliability::Unreliable ||
	    internalPacket->reliability == MafiaNet::Reliability::UnreliableSequenced ||
	    internalPacket->reliability == MafiaNet::Reliability::UnreliableWithAckReceipt
	    //		||
	    //		internalPacket->reliability==UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
	)
	{
		if (unreliableLinkedListHead == 0)
		{
			internalPacket->unreliableNext = internalPacket;
			internalPacket->unreliablePrev = internalPacket;
			unreliableLinkedListHead = internalPacket;
			return;
		}
		internalPacket->unreliableNext = unreliableLinkedListHead;
		internalPacket->unreliablePrev = unreliableLinkedListHead->unreliablePrev;
		internalPacket->unreliablePrev->unreliableNext = internalPacket;
		unreliableLinkedListHead->unreliablePrev = internalPacket;
	}
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::ValidateResendList(void) const
{
	// 	unsigned int count1=0, count2=0;
	// 	for (unsigned int i=0; i < RESEND_BUFFER_ARRAY_LENGTH; i++)
	// 	if (resendBuffer[i])
	// 	count1++;
	//
	// 	if (resendLinkedListHead)
	// 	{
	// 	InternalPacket *internalPacket = resendLinkedListHead;
	// 	do
	// 	{
	// 	count2++;
	// 	internalPacket=internalPacket->resendNext;
	// 	} while (internalPacket!=resendLinkedListHead);
	// 	}
	// 	RakAssert(count1==count2);
	// 	RakAssert(count2<=RESEND_BUFFER_ARRAY_LENGTH);
}
//-------------------------------------------------------------------------------------------------------
bool ReliabilityLayer::ResendBufferOverflow(void) const
{
	int index1 = sendReliableMessageNumberIndex & (uint32_t) RESEND_BUFFER_ARRAY_MASK;
	//	int index2 = (sendReliableMessageNumberIndex+(uint32_t)1) & (uint32_t) RESEND_BUFFER_ARRAY_MASK;
	RakAssert(index1 < RESEND_BUFFER_ARRAY_LENGTH);
	return resendBuffer[index1] != 0; // || resendBuffer[index2]!=0;
}
//-------------------------------------------------------------------------------------------------------
ReliabilityLayer::MessageNumberNode *ReliabilityLayer::GetMessageNumberNodeByDatagramIndex(DatagramSequenceNumberType index, CCTimeType *timeSent, int *wireBytes)
{
	if (datagramHistory.empty())
		return nullptr;

	if (congestionManager.LessThan(index, datagramHistoryPopCount))
		return nullptr;

	DatagramSequenceNumberType offsetIntoList = index - datagramHistoryPopCount;
	if (offsetIntoList >= datagramHistory.size())
		return nullptr;

	*timeSent = datagramHistory[offsetIntoList].timeSent;
	if (wireBytes)
		*wireBytes = datagramHistory[offsetIntoList].wireBytes;
	return datagramHistory[offsetIntoList].head;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::RemoveFromDatagramHistory(DatagramSequenceNumberType index)
{
	DatagramSequenceNumberType offsetIntoList = index - datagramHistoryPopCount;
	MessageNumberNode *mnm = datagramHistory[offsetIntoList].head;
	MessageNumberNode *next;
	while (mnm)
	{
		next = mnm->next;
		delete mnm;
		mnm = next;
	}
	datagramHistory[offsetIntoList].head = 0;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AddFirstToDatagramHistory(DatagramSequenceNumberType datagramNumber, CCTimeType timeSent)
{
	(void) datagramNumber;
	if (datagramHistory.size() > DATAGRAM_MESSAGE_ID_ARRAY_LENGTH)
	{
		RemoveFromDatagramHistory(datagramHistoryPopCount);
		datagramHistory.pop_front();
		datagramHistoryPopCount++;
	}

	datagramHistory.push_back(DatagramHistoryNode(0, timeSent));
	// printf("%p Pushed empty DatagramHistoryNode to datagram history at index %i\n", this, datagramHistory.Size()-1);
}
//-------------------------------------------------------------------------------------------------------
ReliabilityLayer::MessageNumberNode *ReliabilityLayer::AddFirstToDatagramHistory(DatagramSequenceNumberType datagramNumber, DatagramSequenceNumberType messageNumber, CCTimeType timeSent)
{
	(void) datagramNumber;
	//	RakAssert(datagramHistoryPopCount+(unsigned int) datagramHistory.Size()==datagramNumber);
	if (datagramHistory.size() > DATAGRAM_MESSAGE_ID_ARRAY_LENGTH)
	{
		RemoveFromDatagramHistory(datagramHistoryPopCount);
		datagramHistory.pop_front();
		datagramHistoryPopCount++;
	}

	MessageNumberNode *mnm = new MessageNumberNode;
	mnm->next = 0;
	mnm->messageNumber = messageNumber;
	datagramHistory.push_back(DatagramHistoryNode(mnm, timeSent));
	// printf("%p Pushed message %i to DatagramHistoryNode to datagram history at index %i\n", this, messageNumber.val, datagramHistory.Size()-1);
	return mnm;
}
//-------------------------------------------------------------------------------------------------------
ReliabilityLayer::MessageNumberNode *ReliabilityLayer::AddSubsequentToDatagramHistory(MessageNumberNode *messageNumberNode, DatagramSequenceNumberType messageNumber)
{
	messageNumberNode->next = new MessageNumberNode;
	messageNumberNode->next->messageNumber = messageNumber;
	messageNumberNode->next->next = 0;
	return messageNumberNode->next;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AllocInternalPacketData(InternalPacket *internalPacket, InternalPacketRefCountedData **refCounter, unsigned char *externallyAllocatedPtr, unsigned char *ourOffset)
{
	internalPacket->allocationScheme = InternalPacket::REF_COUNTED;
	internalPacket->data = ourOffset;
	if (*refCounter == 0)
	{
		*refCounter = new InternalPacketRefCountedData;
		(*refCounter)->refCount = 1;
		(*refCounter)->sharedDataBlock = externallyAllocatedPtr;
	}
	else
		(*refCounter)->refCount++;
	internalPacket->refCountedData = (*refCounter);
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AllocInternalPacketData(InternalPacket *internalPacket, unsigned char *externallyAllocatedPtr)
{
	internalPacket->allocationScheme = InternalPacket::NORMAL;
	internalPacket->data = externallyAllocatedPtr;
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::AllocInternalPacketData(InternalPacket *internalPacket, unsigned int numBytes, bool allowStack)
{
	if (allowStack && numBytes <= sizeof(internalPacket->stackData))
	{
		internalPacket->allocationScheme = InternalPacket::STACK;
		internalPacket->data = internalPacket->stackData;
	}
	else
	{
		internalPacket->allocationScheme = InternalPacket::NORMAL;
		internalPacket->data = (unsigned char *) std::malloc(numBytes);
	}
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::FreeInternalPacketData(InternalPacket *internalPacket)
{
	if (internalPacket == 0)
		return;

	if (internalPacket->allocationScheme == InternalPacket::REF_COUNTED)
	{
		if (internalPacket->refCountedData == 0)
			return;

		internalPacket->refCountedData->refCount--;
		if (internalPacket->refCountedData->refCount == 0)
		{
			std::free(internalPacket->refCountedData->sharedDataBlock);
			internalPacket->refCountedData->sharedDataBlock = 0;
			delete internalPacket->refCountedData;
			internalPacket->refCountedData = 0;
		}
	}
	else if (internalPacket->allocationScheme == InternalPacket::NORMAL)
	{
		if (internalPacket->data == 0)
			return;

		std::free(internalPacket->data);
		internalPacket->data = 0;
	}
	else
	{
		// Data was on stack
		internalPacket->data = 0;
	}
}
//-------------------------------------------------------------------------------------------------------
unsigned int ReliabilityLayer::GetMaxDatagramSizeExcludingMessageHeaderBytes(void)
{
	// The congestion manager's MTU already excludes the cipher overhead (Reset()).
	return congestionManager.GetMTU() - DatagramHeaderFormat::GetDataHeaderByteLength();
}
//-------------------------------------------------------------------------------------------------------
BitSize_t ReliabilityLayer::GetMaxDatagramSizeExcludingMessageHeaderBits(void)
{
	return BYTES_TO_BITS(GetMaxDatagramSizeExcludingMessageHeaderBytes());
}
//-------------------------------------------------------------------------------------------------------
void ReliabilityLayer::InitHeapWeights(void)
{
	for (int priorityLevel = 0; priorityLevel < static_cast<int>(MafiaNet::NUMBER_OF_PRIORITIES); priorityLevel++)
		outgoingPacketBufferNextWeights[priorityLevel] = (1 << priorityLevel) * priorityLevel + priorityLevel;
}
//-------------------------------------------------------------------------------------------------------
reliabilityHeapWeightType ReliabilityLayer::GetNextWeight(int priorityLevel)
{
	uint64_t next = outgoingPacketBufferNextWeights[priorityLevel];
	if (!outgoingPacketBuffer.empty())
	{
		int peekPL = (int) outgoingPacketBuffer.front().packet->priority;
		reliabilityHeapWeightType weight = outgoingPacketBuffer.front().weight;
		reliabilityHeapWeightType min = weight - (1 << peekPL) * peekPL + peekPL;
		if (next < min)
			next = min + (1 << priorityLevel) * priorityLevel + priorityLevel;
		outgoingPacketBufferNextWeights[priorityLevel] = next + (1 << priorityLevel) * (priorityLevel + 1) + priorityLevel;
	}
	else
	{
		InitHeapWeights();
	}
	return next;
}

//-------------------------------------------------------------------------------------------------------
// #if defined(RELIABILITY_LAYER_NEW_UNDEF_ALLOCATING_QUEUE)
// #pragma pop_macro("new")
// #undef RELIABILITY_LAYER_NEW_UNDEF_ALLOCATING_QUEUE
// #endif
