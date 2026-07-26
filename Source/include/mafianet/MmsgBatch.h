/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file MmsgBatch.h
/// \brief Portable helpers for batched datagram I/O (recvmmsg / sendmmsg).
///
/// The Linux-only syscall glue lives in RakNetSocket2_Berkley.cpp behind the
/// MAFIANET_USE_RECVMMSG / MAFIANET_USE_SENDMMSG build flags. Everything in this
/// header is portable and compiled unconditionally -- including RNS2SendBatch,
/// which only needs RakNetSocket2::SendBatch (whose base implementation works on
/// every platform) -- so the logic that is actually bug-prone is unit-testable
/// even where the mmsg syscalls do not exist (macOS, Windows, the BSDs).

#ifndef __MAFIANET_MMSG_BATCH_H
#define __MAFIANET_MMSG_BATCH_H

#include "mafianet/socket2.h"
#include "mafianet/assert.h"
#include "mafianet/memoryoverride.h"

#include <string.h> // memcpy
#include <stdio.h>  // RAKNET_DEBUG_PRINTF defaults to printf

namespace MafiaNet
{

/// Maximum datagrams coalesced into a single recvmmsg/sendmmsg system call.
static const unsigned MMSG_BATCH_MAX = 64;

/// Upper bound (ms) of the progressive back-off the batched recv loop applies
/// after consecutive recvmmsg failures, so a persistent asynchronous socket
/// error cannot spin the polling thread.
static const unsigned MMSG_ERROR_BACKOFF_MAX_MS = 10;

/// Convert a raw sockaddr (IPv4 or IPv6) into a SystemAddress, reproducing the
/// byte-order handling of the scalar recvfrom path in RNS2_Berkley. \a out
/// receives the address; its port is stored in network order in the address
/// union and mirrored (host order) into debugPort.
///
/// Returns false for an address family this build cannot represent (anything
/// other than AF_INET when RAKNET_SUPPORT_IPV6 is off, or AF_UNSPEC), in which
/// case \a out is set to UNASSIGNED_SYSTEM_ADDRESS rather than left holding
/// whatever the recycled recv struct contained. Callers must not dispatch a
/// datagram whose address failed to decode.
bool SockaddrToSystemAddress(const sockaddr_storage &from, SystemAddress *out);

/// Drive a sendmmsg-style batched send to completion, handling partial sends.
///
/// \a transmit is invoked as transmit(offset, count) to send the messages in
/// [offset, offset+count). It must return:
///   - a positive count of messages accepted this call (advance and continue),
///   - 0 when no further progress is possible right now -- a transient,
///     whole-socket condition such as EAGAIN/ENOBUFS (stop, don't spin), or
///   - a negative errno-style value meaning the message at \a offset itself
///     cannot be sent (a permanent per-message error such as EMSGSIZE or
///     EDESTADDRREQ): it is dropped and the rest of the batch continues.
///
/// The 0-vs-negative split matters: sendmmsg reports both through the same -1,
/// so the caller's transmit() must classify errno. Treating a permanent
/// per-message error as "stop" would silently discard every datagram *after*
/// the bad one, which the scalar Send() loop would have delivered.
///
/// Returns the total number of messages actually sent (0..total), or -- when
/// nothing at all went out and at least one message failed permanently -- the
/// first negative error code seen. Note that the drop-and-continue behaviour
/// means further syscalls may run after that first failure, so errno is not
/// preserved; transmit() should encode the error in its return value if the
/// caller needs it.
template <typename TransmitFn>
int DriveBatchedSend(unsigned total, TransmitFn transmit)
{
	unsigned sent = 0;    // messages actually accepted by the socket
	unsigned offset = 0;  // next message to attempt; may run ahead of `sent`
	int firstError = 0;   // first permanent per-message error seen, if any
	while (offset < total)
	{
		int r = transmit(offset, total - offset);
		if (r > 0)
		{
			// Accepted r messages; retry the remainder from the new offset.
			sent += (unsigned) r;
			offset += (unsigned) r;
			continue;
		}
		if (r == 0)
			break; // no progress possible right now; don't spin
		// r < 0: the message at `offset` is undeliverable. Drop just that one
		// and keep going, so a single bad datagram cannot take the rest of the
		// batch down with it.
		if (firstError == 0)
			firstError = r;
		++offset;
	}
	// Mirror sendmmsg, which reports an error only when no datagram at all went
	// out; otherwise report the progress made.
	if (sent == 0 && firstError != 0)
		return firstError;
	return (int) sent;
}

/// Fan a received batch out to the event handler.
///
/// \a slots[0..allocated) are pre-allocated recv structs whose .data buffers
/// already hold the received bytes (the mmsghdr iovecs pointed at them). For
/// each i in [0,received) with lens[i] > 0 the struct is stamped with the byte
/// count, source address (from addrs[i]), timestamp \a now and \a socket, then
/// handed to handler->OnRNS2Recv. Every other struct -- short reads
/// (lens[i] <= 0), datagrams whose source address failed to decode, and the
/// unused tail [received,allocated) -- is returned via
/// handler->DeallocRNS2RecvStruct so no buffer leaks.
void DispatchRecvBatch(RNS2EventHandler *handler,
                       RNS2RecvStruct **slots, unsigned allocated,
                       const int *lens, const sockaddr_storage *addrs,
                       unsigned received, RakNetSocket2 *socket,
                       MafiaNet::TimeUS now);

/// Accumulates already-prepared datagrams (post-encryption, post packet-loss
/// simulation) destined for a single peer and flushes them via
/// RakNetSocket2::SendBatch -- one sendmmsg per burst on Linux, a plain Send()
/// loop everywhere else.
///
/// Bytes are copied in because the caller's serialization buffer is reused for
/// the next datagram. It is meant to be loop-local: construct it before a send
/// loop, Flush() at the loop's single exit (the destructor also flushes as a
/// backstop), so a datagram is never stranded across ticks.
class RNS2SendBatch
{
public:
	RNS2SendBatch(RakNetSocket2 *socket, const SystemAddress &dest)
		: socket(socket), dest(dest), count(0)
	{
		// The datagram buffers are a single block shared by every batch on this
		// thread (see Slot), so two live batches would silently overwrite each
		// other's payloads. The class is loop-local by construction; this catches
		// a future caller that nests one inside another.
		RakAssert(ThreadBatchLive() == false && "RNS2SendBatch is not reentrant");
		ThreadBatchLive() = true;
	}
	~RNS2SendBatch()
	{
		Flush();
		ThreadBatchLive() = false;
	}

	// Non-copyable, non-movable: a copy would alias the same shared buffers and
	// flush the same datagrams a second time from its destructor.
	RNS2SendBatch(const RNS2SendBatch &) = delete;
	RNS2SendBatch &operator=(const RNS2SendBatch &) = delete;

	/// Append one already-prepared datagram. Oversized datagrams are rejected
	/// rather than truncated: the scalar send path never truncates, and silently
	/// clamping would ship corrupted bytes with no error signal. Callers already
	/// keep datagrams within the MTU (RakAssert in ReliabilityLayer), so this is
	/// a release-build backstop, not the normal path.
	void Add(const char *data, int length)
	{
		RakAssert(length >= 0 && length <= MAXIMUM_MTU_SIZE);
		if (length < 0 || length > MAXIMUM_MTU_SIZE)
		{
			// Drop rather than corrupt. Reliable traffic is resent by the
			// reliability layer; an unreliable datagram is simply lost, which is
			// why the diagnostic below is the only trace it leaves.
			RAKNET_DEBUG_PRINTF("RNS2SendBatch dropped an oversized datagram (%d bytes).\n", length);
			return;
		}
		if (count == MMSG_BATCH_MAX)
			Flush();
		memcpy(Slot(count), data, (size_t) length);
		lengths[count] = length;
		++count;
	}

	void Flush()
	{
		if (count == 0)
			return;
		RNS2_SendParameters sends[MMSG_BATCH_MAX];
		for (unsigned i = 0; i < count; ++i)
		{
			sends[i].data = Slot(i);
			sends[i].length = lengths[i];
			sends[i].systemAddress = dest;
			sends[i].ttl = 0;
		}
		// SendBatch returns a datagram count, or a negative error when nothing at
		// all went out. Either shortfall means datagrams were dropped; the scalar
		// path reports its sendto failures the same way, so don't lose the signal.
		const RNS2SendResult sent = socket->SendBatch(sends, count, _FILE_AND_LINE_);
		// A return above `count` means an override handed back a byte total
		// instead of a datagram count -- the one contract the RNS2_Linux sendmmsg
		// override cannot be unit-tested against, so check it at runtime here.
		RakAssert(sent <= (RNS2SendResult) count && "SendBatch must return a datagram count, not a byte total");
		if (sent < 0 || (unsigned) sent < count)
			RAKNET_DEBUG_PRINTF("SendBatch sent %d of %u datagrams.\n", (int) sent, count);
		// Cleared unconditionally: a datagram the socket refused is dropped here,
		// not carried into the next flush. Reliable traffic is resent by the
		// reliability layer; retrying in place would reorder it against the
		// datagrams queued after it.
		count = 0;
	}

private:
	// One buffer block per thread that actually batches, reused across every
	// connection and tick -- the batch is filled and flushed synchronously
	// within a single UpdateInternal call, so there is no reentrancy. This keeps
	// the send path allocation-free (the old per-tick new[] churned ~95 KB each
	// call) while keeping the block off the stack and out of TLS for the many
	// threads that never send a batch: it is allocated on this thread's first
	// Add() and released when the thread exits.
	static char *Slot(unsigned i)
	{
		struct Block
		{
			char *bytes;
			Block() : bytes(MafiaNet::OP_NEW_ARRAY<char>(MMSG_BATCH_MAX * MAXIMUM_MTU_SIZE, _FILE_AND_LINE_)) {}
			~Block() { MafiaNet::OP_DELETE_ARRAY(bytes, _FILE_AND_LINE_); }
		};
		static thread_local Block block;
		return block.bytes + (size_t) i * MAXIMUM_MTU_SIZE;
	}

	// Debug-only guard against a second live batch on the same thread.
	static bool &ThreadBatchLive()
	{
		static thread_local bool live = false;
		return live;
	}

	RakNetSocket2 *socket;
	SystemAddress dest;
	unsigned count;
	int lengths[MMSG_BATCH_MAX];
};

} // namespace MafiaNet

#endif // __MAFIANET_MMSG_BATCH_H
