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
/// \brief \b [Internal] Datagram reliable, ordered, unordered and sequenced sends.  Flow control.  Message splitting, reassembly, and coalescence.
///


#ifndef __RELIABILITY_LAYER_H
#define __RELIABILITY_LAYER_H

#include "mafianet/export.h"
#include "mafianet/mtu_size.h"
#include "mafianet/socket_layer.h"
#include "mafianet/packet_priority.h"
#include "mafianet/bit_stream.h"
#include "mafianet/internal_packet.h"
#include "mafianet/statistics.h"
#include "mafianet/range_list.h"
#include "mafianet/datagram_gap_tracker.h"
#include "mafianet/object_pool.h"
#include "mafianet/defines.h"
#include "mafianet/bit_stream.h"
#include "mafianet/native_feature_includes.h"
#include "mafianet/session_crypto.h"
#include "mafianet/mtu_black_hole.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/rand.h"
#include "mafianet/socket2.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL != 1
#include "mafianet/cc_udt.h"
#define INCLUDE_TIMESTAMP_WITH_DATAGRAMS 1
#else
#include "mafianet/cc_sliding_window.h"
#define INCLUDE_TIMESTAMP_WITH_DATAGRAMS 0
#endif

/// Number of ordered streams available. You can use up to 32 ordered streams
#define NUMBER_OF_ORDERED_STREAMS 32 // 2^5

namespace MafiaNet
{

/// Forward declarations
class PluginInterface2;
class RakNetRandom;
class RNS2SendBatch; // mmsg_batch.h
typedef uint64_t reliabilityHeapWeightType;

// #med - consider a more suitable name for the class / maybe even make an internal class to SplitPacketChannel?
// The fragments of one split message, keyed by fragment index.
//
// Every field here comes from the remote. Storage grows with the fragments that
// actually arrive, never with the count a fragment claims, so a peer cannot make
// us allocate more than it sends. Add() rejects any fragment that disagrees with
// the first one about the fragment count, and any non-final fragment that is not
// a whole number of bytes: reassembly copies fragments at byte offsets.
class SplitPacketSort
{
private:
	std::map<SplitPacketIndexType, InternalPacket *> m_fragments;
	SplitPacketIndexType m_count;
	SplitPacketIdType m_packetId;

public:
	SplitPacketSort();

	// Records the message's id and fragment count from its first fragment.
	void Init(const InternalPacket *firstFragment);

	bool AllPacketsAdded() const;
	unsigned int GetNumAddedPackets() const;
	SplitPacketIdType GetPacketId() const;

	// In index order. Once AllPacketsAdded(), indices are exactly 0..count-1.
	const std::map<SplitPacketIndexType, InternalPacket *> &Fragments() const;

	// false (packet not taken) for a duplicate or an inconsistent fragment.
	bool Add(InternalPacket *internalPacket);
};

// int SplitPacketIndexComp( SplitPacketIndexType const &key, InternalPacket* const &data );
struct SplitPacketChannel //<SplitPacketChannel>
{
	CCTimeType lastUpdateTime;

	SplitPacketSort splitPacketList;

#if PREALLOCATE_LARGE_MESSAGES == 1
	InternalPacket *returnedPacket;
	bool gotFirstPacket;
	unsigned int stride;
	unsigned int splitPacketsArrived;
#else
	// This is here for progress notifications, since progress notifications return the first packet data, if available
	InternalPacket *firstPacket;
#endif
};

// Helper class
struct BPSTracker
{
	BPSTracker();
	~BPSTracker();
	void Reset();
	inline void Push1(CCTimeType time, uint64_t value1)
	{
		dataQueue.push_back(TimeAndValue2(time, value1));
		total1 += value1;
		lastSec1 += value1;
	}
	//	void Push2(MafiaNet::TimeUS time, uint64_t value1, uint64_t value2);
	inline uint64_t GetBPS1(CCTimeType time)
	{
		(void) time;
		return lastSec1;
	}
	inline uint64_t GetBPS1Threadsafe(CCTimeType time)
	{
		(void) time;
		return lastSec1;
	}
	//	uint64_t GetBPS2(RakNetTimeUS time);
	//	void GetBPS1And2(RakNetTimeUS time, uint64_t &out1, uint64_t &out2);
	uint64_t GetTotal1(void) const;
	//	uint64_t GetTotal2(void) const;

	struct TimeAndValue2
	{
		TimeAndValue2();
		~TimeAndValue2();
		TimeAndValue2(CCTimeType t, uint64_t v1);
		//	TimeAndValue2(MafiaNet::TimeUS t, uint64_t v1, uint64_t v2);
		//	uint64_t value1, value2;
		uint64_t value1;
		CCTimeType time;
	};

	uint64_t total1, lastSec1;
	//	uint64_t total2, lastSec2;
	std::deque<TimeAndValue2> dataQueue;
	void ClearExpired1(CCTimeType time);
	//	void ClearExpired2(MafiaNet::TimeUS time);
};

/// Datagram reliable, ordered, unordered and sequenced sends.  Flow control.  Message splitting, reassembly, and coalescence.
class ReliabilityLayer //<ReliabilityLayer>
{
public:
	// Constructor
	ReliabilityLayer();

	// Destructor
	~ReliabilityLayer();

	/// Resets the layer for reuse. Wipes any session keys: a new connection
	/// installs fresh ones with SetSessionKeys() once its handshake completes.
	/// When the encryption mode is Required, DatagramCipher::kMaxOverhead bytes
	/// of every datagram are reserved for the cipher, even before keys exist.
	void Reset(bool resetVariables, int mtuSize);

	/// The encryption mode of the owning peer. Call before Reset(); it decides
	/// whether Reset() reserves the cipher overhead in the datagram budget.
	/// Survives Reset(). Defaults to Required.
	void SetEncryptionMode(EncryptionMode mode);

	/// Installs freshly derived session keys: from now on every datagram is
	/// sealed on send, and every received datagram must open or is dropped
	/// (and counted in RakNetStatistics::droppedUnauthenticatedDatagrams).
	/// Never re-install keys already used: the send sequence restarts at 0.
	/// The caller wipes its own copy.
	/// \return false, installing nothing, unless the mode is Required and Reset()
	/// reserved the cipher overhead. Until keys are installed, a layer in mode
	/// Required sends nothing and accepts nothing (it never falls back to plaintext).
	bool SetSessionKeys(const SessionKeys &keys);

	/// True once session keys are installed (until the next Reset()).
	bool IsEncrypted() const;

	/// Test-only: copies the installed session keys into \a out (zeroed when there are none).
	/// \return IsEncrypted().
	bool GetSessionKeysForTesting(SessionKeys *out) const;

	/// Test-only: datagrams discarded on send because the mode is Required and no keys are installed.
	uint64_t GetDroppedKeylessSendsForTesting() const;

	/// Test-only: how many reliable message numbers past the lowest one not yet
	/// received this layer is tracking (0 when every number up to the newest
	/// has arrived). A number that never arrives keeps this from returning to 0.
	unsigned int GetReceiveWindowSizeForTesting() const;

	/// Test-only: split messages this layer has fragments of but has not yet
	/// reassembled.
	unsigned int GetPartialSplitMessagesForTesting() const;

	/// Test-only: lower the receive-window bound past which a reliable message
	/// is dropped as hostile (default 1,000,000), so a test can reach it.
	void SetMaxReceiveWindowForTesting(uint32_t messages);

	/// Test-only: reliable message numbers re-split headers have marked received so far: the work
	/// ResolveReSplitCopy has done, which a header's size bounds.
	uint64_t GetReSplitNumbersMarkedForTesting() const;

	/// Set the time, in MS, to use before considering ourselves disconnected after not being able to deliver a reliable packet
	/// Default time is 10,000 or 10 seconds in release and 30,000 or 30 seconds in debug.
	/// \param[in] time Time, in MS
	void SetTimeoutTime(MafiaNet::TimeMS time);

	/// Returns the value passed to SetTimeoutTime. or the default if it was never called
	/// \param[out] the value passed to SetTimeoutTime
	MafiaNet::TimeMS GetTimeoutTime(void);

	/// The connection's current MTU in bytes, including UDP/IP headers. Starts
	/// at the handshake-negotiated size passed to Reset() and steps down the
	/// ladder in mtu_black_hole.h when an in-session path-MTU black hole is
	/// detected. Never rises again for the life of the connection.
	int GetCurrentMtuBytes(void) const;

	/// Packets are read directly from the socket layer and skip the reliability layer because unconnected players do not use the reliability layer
	/// This function takes packet data after a player has been confirmed as connected.
	/// \param[in] buffer The socket data
	/// \param[in] length The length of the socket data
	/// \param[in] systemAddress The player that this data is from
	/// \param[in] messageHandlerList A list of registered plugins
	/// \param[in] mtuSize maximum datagram size
	/// \retval true Success
	/// \retval false Modified packet
	bool HandleSocketReceiveFromConnectedPlayer(
	    const char *buffer, unsigned int length, SystemAddress &systemAddress, std::vector<PluginInterface2 *> &messageHandlerList, int mtuSize,
	    RakNetSocket2 *s, RakNetRandom *rnr, CCTimeType timeRead, BitStream &updateBitStream);

	/// This allocates bytes and writes a user-level message to those bytes.
	/// \param[out] data The message
	/// \return Returns number of BITS put into the buffer
	BitSize_t Receive(unsigned char **data);

	/// Puts data on the send queue
	/// \param[in] data The data to send
	/// \param[in] numberOfBitsToSend The length of \a data in bits
	/// \param[in] priority The priority level for the send
	/// \param[in] reliability The reliability type for the send
	/// \param[in] orderingChannel 0 to 31.  Specifies what channel to use, for relational ordering and sequencing of packets.
	/// \param[in] makeDataCopy If true \a data will be copied.  Otherwise, only a pointer will be stored.
	/// \param[in] MTUSize maximum datagram size
	/// \param[in] currentTime Current time, as per MafiaNet::GetTimeMS()
	/// \param[in] receipt This number will be returned back with ID_SND_RECEIPT_ACKED or ID_SND_RECEIPT_LOSS and is only returned with the reliability types that contain RECEIPT in the name
	/// \return True or false for success or failure.
	bool Send(char *data, BitSize_t numberOfBitsToSend, MafiaNet::Priority priority, MafiaNet::Reliability reliability, unsigned char orderingChannel, bool makeDataCopy, int MTUSize, CCTimeType currentTime, uint32_t receipt);

	/// Call once per game cycle.  Handles internal lists and actually does the send.
	/// \param[in] s the communication  end point
	/// \param[in] systemAddress The Unique Player Identifier who shouldhave sent some packets
	/// \param[in] MTUSize maximum datagram size
	/// \param[in] time current system time
	/// \param[in] maxBitsPerSecond if non-zero, enforces that outgoing bandwidth does not exceed this amount
	/// \param[in] messageHandlerList A list of registered plugins
	void Update(RakNetSocket2 *s, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
	            unsigned bitsPerSecondLimit,
	            std::vector<PluginInterface2 *> &messageHandlerList,
	            RakNetRandom *rnr, BitStream &updateBitStream);

	// #med 0.2.0 - review whether we'd rather have this defined as a private method and declare RakPeer a friend of ReliabilityLayer
	/// @since 0.2.0: added
	/// Same as \see Update() except that outstanding ACKs are ensured to be sent.
	void UpdateAndForceACKs(RakNetSocket2 *s, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
	                        unsigned bitsPerSecondLimit,
	                        std::vector<PluginInterface2 *> &messageHandlerList,
	                        RakNetRandom *rnr, BitStream &updateBitStream);

	/// Were you ever unable to deliver a packet despite retries?
	/// \return true means the connection has been lost.  Otherwise not.
	bool IsDeadConnection(void) const;

	/// Causes IsDeadConnection to return true
	void KillConnection(void);

	/// Get Statistics
	/// \return A pointer to a static struct, filled out with current statistical information.
	RakNetStatistics *GetStatistics(RakNetStatistics *rns);

	///Are we waiting for any data to be sent out or be processed by the player?
	bool IsOutgoingDataWaiting(void);
	bool AreAcksWaiting(void);

	// Set outgoing lag and packet loss properties
	void ApplyNetworkSimulator(double _maxSendBPS, MafiaNet::TimeMS _minExtraPing, MafiaNet::TimeMS _extraPingVariance);

	/// Returns if you previously called ApplyNetworkSimulator
	/// \return If you previously called ApplyNetworkSimulator
	bool IsNetworkSimulatorActive(void);

	void SetSplitMessageProgressInterval(int interval);
	void SetUnreliableTimeout(MafiaNet::TimeMS timeoutMS);
	/// Has a lot of time passed since the last ack
	bool AckTimeout(MafiaNet::Time curTime);
	CCTimeType GetNextSendTime(void) const;
	CCTimeType GetTimeBetweenPackets(void) const;
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
	CCTimeType GetAckPing(void) const;
#endif
	MafiaNet::TimeMS GetTimeLastDatagramArrived(void) const { return timeLastDatagramArrived; }

	// If true, will update time between packets quickly based on ping calculations
	//void SetDoFastThroughputReactions(bool fast);

	// Encoded as numMessages[unsigned int], message1BitLength[unsigned int], message1Data (aligned), ...
	//void GetUndeliveredMessages(MafiaNet::BitStream *messages, int MTUSize);

	/// \internal One entry of the layer's binary min-heaps (send scheduling and
	/// out-of-order buffering), kept in a std::vector by HeapPush/HeapPop.
	/// Entries pop by ascending weight; equal weights pop in push order, by seq,
	/// which comes from a per-heap counter bumped on every push.
	struct HeapEntry
	{
		reliabilityHeapWeightType weight;
		std::uint64_t seq;
		InternalPacket *packet;
	};
	/// \internal Heap comparator for std::push_heap/std::pop_heap: true when
	/// \a a pops after \a b, which keeps the smallest (weight, seq) at front().
	struct HeapEntryPopsLater
	{
		bool operator()(const HeapEntry &a, const HeapEntry &b) const
		{
			return a.weight > b.weight || (a.weight == b.weight && a.seq > b.seq);
		}
	};
	/// \internal Adds \a packet to \a heap under \a weight, stamped with nextSeq++.
	static void HeapPush(std::vector<HeapEntry> &heap, std::uint64_t &nextSeq, reliabilityHeapWeightType weight, InternalPacket *packet)
	{
		heap.push_back(HeapEntry{weight, nextSeq++, packet});
		std::push_heap(heap.begin(), heap.end(), HeapEntryPopsLater());
	}
	/// \internal Removes and returns the packet at front(). \pre !heap.empty()
	static InternalPacket *HeapPop(std::vector<HeapEntry> &heap)
	{
		std::pop_heap(heap.begin(), heap.end(), HeapEntryPopsLater());
		InternalPacket *packet = heap.back().packet;
		heap.pop_back();
		return packet;
	}

private:
	friend class SimulatedNetwork; // samples the sender budget between simulated ticks
	// Freshly reset per connection. Distinguish deadlines shortened by NAK from ordinary RTO.
	uint64_t nakRetryBytes, rtoRetryBytes;
	uint32_t nakRetryWindowCuts, rtoRetryWindowCuts;
	/// Send the contents of a bitstream to the socket
	/// \param[in] s The socket used for sending data
	/// \param[in] systemAddress The address and port to send to
	/// \param[in] bitStream The data to send.
	/// \param[in] sendBatch If non-null, the fully-prepared datagram is appended
	///            to this batch (coalesced into a sendmmsg) instead of being sent
	///            immediately. All other processing (encryption, loss simulation,
	///            metrics) still happens per datagram. Non-null only where the
	///            batched syscalls exist (Linux).
	void SendBitStream(RakNetSocket2 *s, SystemAddress &systemAddress, MafiaNet::BitStream *bitStream, RakNetRandom *rnr, CCTimeType currentTime, RNS2SendBatch *sendBatch = nullptr);

	///Parse an internalPacket and create a bitstream to represent this data
	/// \return Returns number of bits used
	BitSize_t WriteToBitStreamFromInternalPacket(MafiaNet::BitStream *bitStream, const InternalPacket *const internalPacket, CCTimeType curTime);


	/// Parse a bitstream and create an internal packet to represent this data
	InternalPacket *CreateInternalPacketFromBitStream(MafiaNet::BitStream *bitStream, CCTimeType time);

	/// Does what the function name says
	unsigned RemovePacketFromResendListAndDeleteOlderReliableSequenced(const MessageNumberType messageNumber, CCTimeType time, std::vector<PluginInterface2 *> &messageHandlerList, const SystemAddress &systemAddress);

	/// Acknowledge receipt of the packet with the specified messageNumber
	void SendAcknowledgementPacket(const DatagramSequenceNumberType messageNumber, CCTimeType time);

	/// This will return true if we should not send at this time
	bool IsSendThrottled(int MTUSize);

	/// We lost a packet
	void UpdateWindowFromPacketloss(CCTimeType time);

	/// Increase the window size
	void UpdateWindowFromAck(CCTimeType time);

	/// Parse an internalPacket and figure out how many header bits would be written.  Returns that number
	BitSize_t GetMaxMessageHeaderLengthBits(void);
	BitSize_t GetMessageHeaderLengthBits(const InternalPacket *const internalPacket);

	/// Returns true if newPacketOrderingIndex is older than the waitingForPacketOrderingIndex
	bool IsOlderOrderedPacket(OrderingIndexType newPacketOrderingIndex, OrderingIndexType waitingForPacketOrderingIndex);

	/// Split the passed packet into chunks under MTU_SIZE bytes (including headers) and save those new chunks
	void SplitPacket(InternalPacket *internalPacket);

	/// In-session path-MTU black-hole recovery: drop currentMtuBytes one rung
	/// down the ladder in mtu_black_hole.h, shrink the congestion manager's
	/// datagram ceiling to match, and re-split every queued message that no
	/// longer fits. Called from Update() when a too-large reliable packet has
	/// burnt its resend budget without an ack (ShouldStepDownMtu).
	void StepDownMtuAfterBlackHole(void);

	/// Rebuild and re-split, at the current (lowered) MTU, every queued message
	/// with a packet too large for one datagram: split messages are
	/// reconstructed from the shared data block their fragments reference and
	/// re-split under a fresh splitPacketId, with their ordering indices
	/// preserved; oversized unsplit reliable messages are simply split;
	/// oversized unsplit unreliable messages are dropped, as the network was
	/// already free to drop them. Must only run when packetsToSendThisUpdate is
	/// empty, since it frees queued packets.
	///
	/// The old copy's unacknowledged packets are dropped with their reliable
	/// message numbers, which are never sent again. The receiver may have none,
	/// some or all of them (a false positive: only the acks were lost). So a
	/// reliable copy carries a re-split header (AddReSplitHeader) naming those
	/// numbers, and the receiver (ResolveReSplitCopy) closes them -- no number
	/// stays a hole in its receive window (#92) -- and delivers the copy only if
	/// the old one cannot have been delivered (#93).
	void ReSplitOversizedMessages(void);

	/// Prefix \a rebuilt's data with the re-split header and mark it resplitCopy.
	/// \a oldCopyIncomplete: a packet of the old copy was never sent.
	/// \a retiredNumbers: the reliable message numbers of its unacknowledged packets.
	void AddReSplitHeader(InternalPacket *rebuilt, bool oldCopyIncomplete, bool wasSplit, SplitPacketIdType oldSplitId,
	                      std::vector<MessageNumberType> &retiredNumbers);

	enum ReSplitVerdict
	{
		RESPLIT_DELIVER,
		RESPLIT_DISCARD,  // The copy it replaces was delivered: a duplicate.
		RESPLIT_MALFORMED // The header does not parse or is out of bounds: dropped.
	};
	/// Receiver side of a reassembled resplitCopy message: mark every reliable
	/// message number its header retires as received, discard the remains of the
	/// old copy, and strip the header (each nested one in turn). Deliver when one
	/// of those numbers had not arrived -- the old copy's packet under it will
	/// now be dropped as a duplicate, so the old copy can never complete -- or
	/// when the old copy was never fully sent; discard when all had arrived: the
	/// old copy was then complete and was handled when it was.
	/// Every level is checked before anything is marked: at most one level per
	/// step-down, no more numbers than the old copy can have been sent under
	/// (bounded by the message's size), and every number older than the copy's
	/// own. A header's work is thus bounded by its message's size.
	ReSplitVerdict ResolveReSplitCopy(InternalPacket *internalPacket);

	/// Whether \a number is a reliable message number the receive window would accept.
	bool IsInReceiveWindow(MessageNumberType number) const;

	/// Record \a number as received in the receive window. True if it had not been.
	bool MarkReliableMessageNumberReceived(MessageNumberType number);

	/// Free the fragments received so far of split message \a id, if any.
	void DiscardSplitPacketChannel(SplitPacketIdType id);

	static bool IsReliableForReSplit(MafiaNet::Reliability reliability);

	/// Insert a packet into the split packet list
	void InsertIntoSplitPacketList(InternalPacket *internalPacket, CCTimeType time);

	/// Take all split chunks with the specified splitPacketId and try to reconstruct a packet. If we can, allocate and return it.  Otherwise return 0
	InternalPacket *BuildPacketFromSplitPacketList(SplitPacketIdType inSplitPacketId, CCTimeType time,
	                                               RakNetSocket2 *s, SystemAddress &systemAddress, RakNetRandom *rnr, BitStream &updateBitStream);
	InternalPacket *BuildPacketFromSplitPacketList(SplitPacketChannel *splitPacketChannel, CCTimeType time);

	/// Delete any unreliable split packets that have long since expired
	//void DeleteOldUnreliableSplitPackets( CCTimeType time );

	/// Creates a copy of the specified internal packet with data copied from the original starting at dataByteOffset for dataByteLength bytes.
	/// Does not copy any split data parameters as that information is always generated does not have any reason to be copied
	InternalPacket *CreateInternalPacketCopy(InternalPacket *original, int dataByteOffset, int dataByteLength, CCTimeType time);

	/// Add the internal packet to the ordering list in order based on order index
	// void AddToOrderingList( InternalPacket * internalPacket );

	/// Inserts a packet into the resend list in order
	void InsertPacketIntoResendList(InternalPacket *internalPacket, CCTimeType time, bool firstResend, bool modifyUnacknowledgedBytes);

	/// Memory handling
	void FreeMemory(bool freeAllImmediately);

	/// Memory handling
	void FreeThreadSafeMemory(void);

	// Initialize the variables
	void InitializeVariables(void);

	/// Given the current time, is this time so old that we should consider it a timeout?
	bool IsExpiredTime(unsigned int input, CCTimeType currentTime) const;

	// Make it so we don't do resends within a minimum threshold of time
	void UpdateNextActionTime(void);

	void UpdateInternal(RakNetSocket2 *s, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
	                    unsigned bitsPerSecondLimit,
	                    std::vector<PluginInterface2 *> &messageHandlerList,
	                    RakNetRandom *rnr, BitStream &updateBitStream, bool forceSendACKs);

	/// Does this packet number represent a packet that was skipped (out of order?)
	//unsigned int IsReceivedPacketHole(unsigned int input, MafiaNet::TimeMS currentTime) const;

	/// Skip an element in the received packets list
	//unsigned int MakeReceivedPacketHole(unsigned int input) const;

	/// How many elements are waiting to be resent?
	unsigned int GetResendListDataSize(void) const;

	/// Update all memory which is not threadsafe
	void UpdateThreadedMemory(void);

	void CalculateHistogramAckSize(void);

	// Used ONLY for MafiaNet::Reliability::ReliableOrdered
	// MafiaNet::Reliability::ReliableSequenced just returns the newest one
	std::deque<InternalPacket *> outputQueue;
	// Every this many fragments of a split message, ID_DOWNLOAD_PROGRESS; 0 (until SetSplitMessageProgressInterval) for none.
	int splitMessageProgressInterval = 0;
	CCTimeType unreliableTimeout;

	struct MessageNumberNode
	{
		DatagramSequenceNumberType messageNumber;
		MessageNumberNode *next;
	};
	struct DatagramHistoryNode
	{
		DatagramHistoryNode()
		    : head(0), timeSent(0), wireBytes(0) {}
		DatagramHistoryNode(MessageNumberNode *_head, CCTimeType ts)
		    : head(_head), timeSent(ts), wireBytes(0)
		{
		}
		MessageNumberNode *head;
		CCTimeType timeSent;
		// The datagram's size on the wire, UDP/IP headers included, estimated
		// as Update() estimates a packet's for the black-hole check: an ack for
		// it is evidence that the path carries datagrams of this size.
		int wireBytes;
	};
	// Queue length is programmatically restricted to DATAGRAM_MESSAGE_ID_ARRAY_LENGTH
	// This is essentially an O(1) lookup to get a DatagramHistoryNode given an index
	// datagramHistory holds a linked list of MessageNumberNode. Each MessageNumberNode refers to one element in resendList which can be cleared on an ack.
	std::deque<DatagramHistoryNode> datagramHistory;

	struct UnreliableWithAckReceiptNode
	{
		UnreliableWithAckReceiptNode() {}
		UnreliableWithAckReceiptNode(DatagramSequenceNumberType _datagramNumber, uint32_t _sendReceiptSerial, MafiaNet::TimeUS _nextActionTime)
		    : datagramNumber(_datagramNumber), sendReceiptSerial(_sendReceiptSerial), nextActionTime(_nextActionTime)
		{
		}
		DatagramSequenceNumberType datagramNumber;
		uint32_t sendReceiptSerial;
		MafiaNet::TimeUS nextActionTime;
	};
	std::vector<UnreliableWithAckReceiptNode> unreliableWithAckReceiptHistory;

	void RemoveFromDatagramHistory(DatagramSequenceNumberType index);
	MessageNumberNode *GetMessageNumberNodeByDatagramIndex(DatagramSequenceNumberType index, CCTimeType *timeSent, int *wireBytes = nullptr);
	void AddFirstToDatagramHistory(DatagramSequenceNumberType datagramNumber, CCTimeType timeSent);
	MessageNumberNode *AddFirstToDatagramHistory(DatagramSequenceNumberType datagramNumber, DatagramSequenceNumberType messageNumber, CCTimeType timeSent);
	MessageNumberNode *AddSubsequentToDatagramHistory(MessageNumberNode *messageNumberNode, DatagramSequenceNumberType messageNumber);
	DatagramSequenceNumberType datagramHistoryPopCount;

	// Every message sent, received or split takes one: plain new/delete cost up
	// to 20% in the ReliabilityLayer_* benchmarks, so they are recycled.
	ObjectPool<InternalPacket> internalPacketPool;
	InternalPacket *resendBuffer[RESEND_BUFFER_ARRAY_LENGTH];
	InternalPacket *resendLinkedListHead;
	InternalPacket *unreliableLinkedListHead;
	void RemoveFromUnreliableLinkedList(InternalPacket *internalPacket);
	void AddToUnreliableLinkedList(InternalPacket *internalPacket);
	//	unsigned int numPacketsOnResendBuffer;
	//unsigned int blockWindowIncreaseUntilTime;
	//	RangeList<DatagramSequenceNumberType> acknowlegements;
	// Resend list is a tree of packets we need to resend

	// Set to the current time when the resend queue is no longer empty
	// Set to zero when it becomes empty
	// Set to the current time if it is not zero, and we get incoming data
	// If the current time - timeResendQueueNonEmpty is greater than a threshold, we are disconnected
	//	CCTimeType timeResendQueueNonEmpty;
	MafiaNet::TimeMS timeLastDatagramArrived;


	// If we backoff due to packetloss, don't remeasure until all waiting resends have gone out or else we overcount
	//	bool packetlossThisSample;
	//	int backoffThisSample;
	//	unsigned packetlossThisSampleResendCount;
	//	CCTimeType lastPacketlossTime;

	std::vector<HeapEntry> outgoingPacketBuffer;   // min-heap, see HeapPush/HeapPop
	std::uint64_t outgoingPacketBufferNextSeq = 0; // never reset: ties stay FIFO across Reset()
	reliabilityHeapWeightType outgoingPacketBufferNextWeights[MafiaNet::NUMBER_OF_PRIORITIES];
	void InitHeapWeights(void);
	reliabilityHeapWeightType GetNextWeight(int priorityLevel);
	//	unsigned int messageInSendBuffer[MafiaNet::NUMBER_OF_PRIORITIES];
	//	double bytesInSendBuffer[MafiaNet::NUMBER_OF_PRIORITIES];


	// Partially reassembled split messages, keyed by splitPacketId. Owns the channels.
	std::map<SplitPacketIdType, SplitPacketChannel *> splitPacketChannelList;

	MessageNumberType sendReliableMessageNumberIndex;
	MessageNumberType internalOrderIndex;
	//unsigned int windowSize;
	//MafiaNet::BitStream updateBitStream;
	bool deadConnection, cheater;
	SplitPacketIdType splitPacketId;
	MafiaNet::TimeMS timeoutTime; // How long to wait in MS before timing someone out
	//int MAX_AVERAGE_PACKETS_PER_SECOND; // Name says it all
	//	int RECEIVED_PACKET_LOG_LENGTH, requestedReceivedPacketLogLength; // How big the receivedPackets array is
	//	unsigned int *receivedPackets;
	RakNetStatistics statistics;

	// Algorithm for blending ordered and sequenced on the same channel:
	// 1. Each ordered message transmits OrderingIndexType orderedWriteIndex. There are NUMBER_OF_ORDERED_STREAMS independent values of these. The value
	//    starts at 0. Every time an ordered message is sent, the value increments by 1
	// 2. Each sequenced message contains the current value of orderedWriteIndex for that channel, and additionally OrderingIndexType sequencedWriteIndex.
	//    sequencedWriteIndex resets to 0 every time orderedWriteIndex increments. It increments by 1 every time a sequenced message is sent.
	// 3. The receiver maintains the next expected value for the orderedWriteIndex, stored in orderedReadIndex.
	// 4. As messages arrive:
	//    If a message has the current ordering index, and is sequenced, and is < the current highest sequence value, discard
	//    If a message has the current ordering index, and is sequenced, and is >= the current highest sequence value, return immediately
	//    If a message has a greater ordering index, and is sequenced or ordered, buffer it
	//    If a message has the current ordering index, and is ordered, buffer, then push off messages from buffer
	// 5. Pushing off messages from buffer:
	//    Messages in buffer are put in a minheap. The value of each node is calculated such that messages are returned:
	//    A. (lowest ordering index, lowest sequence index)
	//    B. (lowest ordering index, no sequence index)
	//    C. equal weights (copies of one ordered message, repeated sequenced indices) in arrival order
	//    Messages are pushed off until the heap is empty, or the next message to be returned does not preserve the ordered index
	//    For an empty heap, the heap weight should start at the lowest value based on the next expected ordering index, to avoid variable overflow

	// Sender increments this by 1 for every ordered message sent
	OrderingIndexType orderedWriteIndex[NUMBER_OF_ORDERED_STREAMS];
	// Sender increments by 1 for every sequenced message sent. Resets to 0 when an ordered message is sent
	OrderingIndexType sequencedWriteIndex[NUMBER_OF_ORDERED_STREAMS];
	// Next expected index for ordered messages.
	OrderingIndexType orderedReadIndex[NUMBER_OF_ORDERED_STREAMS];
	// Highest value received for sequencedWriteIndex for the current value of orderedReadIndex on the same channel.
	OrderingIndexType highestSequencedReadIndex[NUMBER_OF_ORDERED_STREAMS];
	std::vector<HeapEntry> orderingHeaps[NUMBER_OF_ORDERED_STREAMS];   // min-heaps, see HeapPush/HeapPop
	std::uint64_t orderingHeapNextSeq[NUMBER_OF_ORDERED_STREAMS] = {}; // never reset: ties stay FIFO across Reset()
	OrderingIndexType heapIndexOffsets[NUMBER_OF_ORDERED_STREAMS];


	//	CCTimeType histogramStart;
	//	unsigned histogramBitsSent;


	/// Memory-efficient receivedPackets algorithm:
	/// receivedPacketsBaseIndex is the packet number we are expecting
	/// Everything under receivedPacketsBaseIndex is a packet we already got
	/// Everything over receivedPacketsBaseIndex is stored in hasReceivedPacketQueue
	/// It stores the time to stop waiting for a particular packet number, where the packet number is receivedPacketsBaseIndex + the index into the queue
	/// If 0, we got got that packet.  Otherwise, the time to give up waiting for that packet.
	/// If we get a packet number where (receivedPacketsBaseIndex-packetNumber) is less than half the range of receivedPacketsBaseIndex then it is a duplicate
	/// Otherwise, it is a duplicate packet (and ignore it).
	std::deque<bool> hasReceivedPacketQueue;
	DatagramSequenceNumberType receivedPacketsBaseIndex;
	bool resetReceivedPackets;

	CCTimeType lastUpdateTime;
	CCTimeType timeBetweenPackets, nextSendTime;
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
	CCTimeType ackPing;
#endif
	//	CCTimeType ackPingSamples[ACK_PING_SAMPLES_SIZE]; // Must be range of unsigned char to wrap ackPingIndex properly
	CCTimeType ackPingSum;
	unsigned char ackPingIndex;
	//CCTimeType nextLowestPingReset;
	RemoteSystemTimeType remoteSystemTime;
	//	bool continuousSend;
	//	CCTimeType lastTimeBetweenPacketsIncrease,lastTimeBetweenPacketsDecrease;
	// Limit changes in throughput to once per ping - otherwise even if lag starts we don't know about it
	// In the meantime the connection is flooded and overrun.
	CCTimeType nextAllowedThroughputSample;
	bool bandwidthExceededStatistic;

	// If Update::maxBitsPerSecond > 0, then throughputCapCountdown is used as a timer to prevent sends for some amount of time after each send, depending on
	// the amount of data sent
	long long throughputCapCountdown;

	unsigned receivePacketCount;

#ifdef _DEBUG
	struct DataAndTime //<InternalPacket>
	{
		RakNetSocket2 *s;
		char data[MAXIMUM_MTU_SIZE];
		unsigned int length;
		MafiaNet::TimeMS sendTime;
		//	SystemAddress systemAddress;
		unsigned int extraSocketOptions;
	};
	std::deque<DataAndTime *> delayList;

	// Internet simulator
	double packetloss;
	MafiaNet::TimeMS minExtraPing, extraPingVariance;
#endif

	int64_t outgoingCreditBitMicros; // One bit of new payload costs one million units.

	CCTimeType nextAckTimeToSend;


#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
	MafiaNet::CCRakNetSlidingWindow congestionManager;
#else
	MafiaNet::CCRakNetUDT congestionManager;
#endif

	// Current wire MTU in bytes including UDP/IP headers. Seeded from Reset()'s
	// mtuSize, stepped down by black-hole detection. See GetCurrentMtuBytes().
	int currentMtuBytes;
	// Failed transmissions of too-large datagrams since one of their size was
	// last acknowledged: the size-specific half of the black-hole signature.
	MtuBlackHoleEvidence mtuBlackHoleEvidence;
	// Step-downs since Reset(). RakNetStatistics::mtuStepDowns.
	uint32_t mtuStepDowns;
	// The reliable-message receive window: a message further than this past the
	// lowest number not yet received is dropped as hostile. 1,000,000 outside tests.
	uint32_t maxReceiveWindow;
	// GetReSplitNumbersMarkedForTesting().
	uint64_t reSplitNumbersMarked;


	uint32_t unacknowledgedBytes;

	bool ResendBufferOverflow(void) const;
	void ValidateResendList(void) const;
	void ResetPacketsAndDatagrams(void);
	void PushPacket(CCTimeType time, InternalPacket *internalPacket, bool isReliable);
	void PushDatagram(void);
	bool TagMostRecentPushAsSecondOfPacketPair(void);
	void ClearPacketsAndDatagrams(void);
	void MoveToListHead(InternalPacket *internalPacket);
	void RemoveFromList(InternalPacket *internalPacket, bool modifyUnacknowledgedBytes);
	void AddToListTail(InternalPacket *internalPacket, bool modifyUnacknowledgedBytes);
	void PopListHead(bool modifyUnacknowledgedBytes);
	bool IsResendQueueEmpty(void) const;
	void SendACKs(RakNetSocket2 *s, SystemAddress &systemAddress, CCTimeType time, RakNetRandom *rnr, BitStream &updateBitStream);

	std::vector<InternalPacket *> packetsToSendThisUpdate;
	std::vector<bool> packetsToDeallocThisUpdate;
	// boundary is in packetsToSendThisUpdate, inclusive
	std::vector<unsigned int> packetsToSendThisUpdateDatagramBoundaries;
	std::vector<bool> datagramsToSendThisUpdateIsPair;
	std::vector<unsigned int> datagramSizesInBytes;
	BitSize_t datagramSizeSoFar;
	BitSize_t allDatagramSizesSoFar;
	double totalUserDataBytesAcked;
	CCTimeType timeOfLastContinualSend;
	CCTimeType timeToNextUnreliableCull;

	// This doesn't need to be a member, but I do it to avoid reallocations
	RangeList<DatagramSequenceNumberType> incomingAcks;

	// Every 16 datagrams, we make sure the 17th datagram goes out the same update tick, and is the same size as the 16th
	int countdownToNextPacketPair;
	InternalPacket *AllocateFromInternalPacketPool(void);
	void ReleaseToInternalPacketPool(InternalPacket *ip);

	RangeList<DatagramSequenceNumberType> acknowlegements;
	RangeList<DatagramSequenceNumberType> NAKs;
	DatagramGapTracker datagramGaps;
	bool remoteSystemNeedsBAndAS;

	unsigned int GetMaxDatagramSizeExcludingMessageHeaderBytes(void);
	BitSize_t GetMaxDatagramSizeExcludingMessageHeaderBits(void);

	// ourOffset refers to a section within externallyAllocatedPtr. Do not deallocate externallyAllocatedPtr until all references are lost
	void AllocInternalPacketData(InternalPacket *internalPacket, InternalPacketRefCountedData **refCounter, unsigned char *externallyAllocatedPtr, unsigned char *ourOffset);
	// Set the data pointer to externallyAllocatedPtr, do not allocate
	void AllocInternalPacketData(InternalPacket *internalPacket, unsigned char *externallyAllocatedPtr);
	// Allocate new
	void AllocInternalPacketData(InternalPacket *internalPacket, unsigned int numBytes, bool allowStack);
	void FreeInternalPacketData(InternalPacket *internalPacket);

	BPSTracker bpsMetrics[RNS_PER_SECOND_METRICS_COUNT];
	CCTimeType lastBpsClear;

	// Datagram protection. encryptionMode survives Reset(); the cipher's keys do not.
	DatagramCipher datagramCipher;
	EncryptionMode encryptionMode;
	// Bytes of every datagram reserved for the cipher: kMaxOverhead in mode
	// Required, 0 in Plaintext. Fixed by Reset(); counted once, in the MTU
	// handed to the congestion manager (Reset() and StepDownMtuAfterBlackHole()).
	int cipherOverheadBytes;
	// Received datagrams that failed to open (forged, corrupted, replayed, too old).
	uint64_t droppedUnauthenticatedDatagrams;
	// Datagrams SendBitStream discarded because the mode is Required and no keys are installed.
	uint64_t droppedKeylessSends;
};

} // namespace MafiaNet

#endif
