/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file MmsgBatch.h
/// \brief Portable helpers for batched datagram I/O (recvmmsg / sendmmsg).
///
/// Batching is always on where the syscalls exist; there is no build option to
/// turn it on or off. The Linux-only
/// syscall glue lives in RakNetSocket2_Berkley.cpp and RakNetSocket2.cpp behind
/// that single guard.
///
/// Everything in this header is portable and compiled unconditionally --
/// including RNS2SendBatch, which only needs RakNetSocket2::SendBatch (whose
/// base implementation works on every platform) -- so the logic that is actually
/// bug-prone is unit-testable even where the mmsg syscalls do not exist (macOS,
/// Windows, the BSDs).

#ifndef __MAFIANET_MMSG_BATCH_H
#define __MAFIANET_MMSG_BATCH_H

#include "mafianet/socket2.h"
#include "mafianet/assert.h"
#include "mafianet/memoryoverride.h"

#include <string.h> // memcpy
#include <stdio.h>  // RAKNET_DEBUG_PRINTF defaults to printf
#include <errno.h>  // error numbers classified by ClassifySendmmsgErrno

/// Diagnostics for dropped datagrams, debug builds only. RAKNET_DEBUG_PRINTF
/// resolves to a plain printf in *every* configuration, and both call sites
/// below sit on the network thread's send path -- a routine ENOBUFS burst on a
/// loaded server would turn them into unbounded, stdout-lock-taking spam inside
/// the very loop batching exists to speed up. Staying silent in a shipping
/// build matches the scalar send path, which ignores an individual sendto
/// failure without a word.
#if defined(_DEBUG)
#define MMSG_BATCH_DEBUG_PRINTF(...) RAKNET_DEBUG_PRINTF(__VA_ARGS__)
#else
#define MMSG_BATCH_DEBUG_PRINTF(...) ((void) 0)
#endif

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

/// Map the errno of a failed sendmmsg(2) onto DriveBatchedSend's transmit()
/// return contract. sendmmsg funnels two opposite conditions through the same
/// -1, so this split is the whole correctness of the batched send path:
///   - transient and socket-wide (the send buffer is full, or the call was
///     interrupted) -> 0, "no progress possible right now". The messages
///     themselves are fine; stop rather than burn a syscall per remaining
///     datagram. Reliable traffic is resent by the reliability layer on a later
///     tick.
///   - anything else (EMSGSIZE, EINVAL, EDESTADDRREQ, an ICMP error posted
///     against a prior send) -> -err, a property of the message at the current
///     offset. DriveBatchedSend drops that one datagram and still ships the
///     rest; treating it as "stop" would silently discard every datagram after
///     the bad one, which the scalar Send() loop would have delivered.
///
/// Free function rather than inline in the override so the mapping is unit
/// testable without a real socket -- it is the one part of the sendmmsg glue
/// where getting it backwards loses traffic silently.
inline int ClassifySendmmsgErrno(int err)
{
	// A caller that hands us a non-error (errno unset, or a nonsensical
	// negative) gets "stop": it cannot be attributed to a single message, and
	// stopping can never spin.
	if (err <= 0)
		return 0;
	// ENOMEM joins ENOBUFS here: sendmsg(2) lists both as "no memory/buffer space
	// available", a whole-socket condition that clears on its own. Classifying it
	// as permanent would drop one healthy datagram per remaining slot and burn a
	// syscall doing it.
	if (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS || err == ENOMEM || err == EINTR)
		return 0;
	return -err;
}

/// Whether \a err from a failed recvmmsg/sendmmsg means the syscall does not
/// exist on this system, as opposed to something wrong with the socket or the
/// datagram.
///
/// ENOSYS only, deliberately. It is the unambiguous "not implemented" signal:
/// a kernel older than the syscall, a seccomp profile or sandbox that filters
/// it (gVisor, restrictive container runtimes), or user-mode emulation. EPERM
/// is NOT included even though some seccomp policies return it, because a
/// firewall rejecting one destination reports EPERM too -- latching batching
/// off process-wide for that would be badly wrong.
///
/// Compiled everywhere so the classification is unit testable off Linux.
inline bool MmsgSyscallMissing(int err)
{
#if defined(ENOSYS)
	return err == ENOSYS;
#else
	(void) err;
	return false;
#endif
}

/// Process-wide latch: has recvmmsg/sendmmsg been observed to be unavailable?
///
/// Batching is not optional at build time, so a system that filters these
/// syscalls needs a *runtime* fallback -- without one, every send would be
/// dropped a datagram at a time and the recv loop would never surface a packet,
/// i.e. total loss of networking on an otherwise healthy host. Both call sites
/// fall back to the portable per-datagram paths once this latches.
///
/// Availability is a property of the kernel/sandbox, not of a socket, so one
/// latch for the process is right. It is one-way (never cleared): the condition
/// cannot change while the process runs, and never clearing keeps it free of
/// races beyond the single relaxed store.
bool MmsgUnavailable(void);
void MarkMmsgUnavailable(void);

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

/// Compact the recv-slot array after a batch has been dispatched.
///
/// The batched recv loop keeps its RNS2RecvStruct slots across passes: only the
/// slots that actually received a datagram change hands (DispatchRecvBatch gives
/// them to the event handler), and the untouched tail is carried over so the
/// steady state costs one alloc/free round trip per datagram rather than
/// MMSG_BATCH_MAX per pass. \a consumed slots have been handed away; the
/// survivors are shifted down to the front of \a slots, preserving their order,
/// and the new slot count is returned.
///
/// \a consumed is clamped to \a allocated: recvmmsg cannot report more messages
/// than the vlen it was given, but getting this wrong would read off the end of
/// the array, so the guard is explicit rather than assumed.
///
/// Free function so the carry-over arithmetic -- the subtlest part of the
/// batched recv loop, and the part that only ever runs on Linux -- is unit
/// testable on every platform without a socket.
unsigned CompactRecvSlots(RNS2RecvStruct **slots, unsigned allocated, unsigned consumed);

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
			// reliability layer; an unreliable datagram is simply lost. The
			// RakAssert above and the diagnostic below are both debug-only, so in
			// a shipping build this drop is silent by design.
			MMSG_BATCH_DEBUG_PRINTF("RNS2SendBatch dropped an oversized datagram (%d bytes).\n", length);
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
			MMSG_BATCH_DEBUG_PRINTF("SendBatch sent %d of %u datagrams.\n", (int) sent, count);
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
	//
	// Defined in MmsgBatch.cpp, deliberately NOT inline here. Both hold
	// thread_local state, and this is a public header, so a consumer could
	// instantiate RNS2SendBatch in its own translation unit. Inline definitions
	// would then give that consumer its own copy of the buffer block and of the
	// reentrancy flag across a shared-library boundary (Windows DLLs,
	// -fvisibility=hidden), silently disarming the guard below and handing the two
	// copies separate buffers. One out-of-line definition in the library keeps a
	// single instance per thread, whoever calls.
	static char *Slot(unsigned i);

	// Debug-only guard against a second live batch on the same thread.
	static bool &ThreadBatchLive();

	RakNetSocket2 *socket;
	SystemAddress dest;
	unsigned count;
	int lengths[MMSG_BATCH_MAX];
};

} // namespace MafiaNet

#endif // __MAFIANET_MMSG_BATCH_H
