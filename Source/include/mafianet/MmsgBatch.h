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
/// MAFIANET_USE_RECVMMSG / MAFIANET_USE_SENDMMSG build flags. The logic that is
/// actually bug-prone -- the sendmmsg partial-send resume loop, the byte-order
/// handling when turning a raw sockaddr into a SystemAddress, and fanning a
/// received batch out to the event handler -- is factored out here so it is
/// portable and unit-testable on platforms that lack the mmsg syscalls (macOS,
/// Windows, the BSDs).

#ifndef __MAFIANET_MMSG_BATCH_H
#define __MAFIANET_MMSG_BATCH_H

#include "mafianet/socket2.h"
#include "mafianet/assert.h"

#include <string.h> // memcpy

namespace MafiaNet
{

/// Maximum datagrams coalesced into a single recvmmsg/sendmmsg system call.
static const unsigned MMSG_BATCH_MAX = 64;

/// Convert a raw sockaddr (IPv4 or IPv6) into a SystemAddress, reproducing the
/// byte-order handling of the scalar recvfrom path in RNS2_Berkley. \a out
/// receives the address; its port is stored in network order in the address
/// union and mirrored (host order) into debugPort.
void SockaddrToSystemAddress(const sockaddr_storage &from, SystemAddress *out);

/// Drive a sendmmsg-style batched send to completion, handling partial sends.
///
/// \a transmit is invoked as transmit(offset, count) to send the messages in
/// [offset, offset+count). It must return:
///   - a positive count of messages accepted this call (advance and continue),
///   - 0 to signal no further progress is possible right now (stop), or
///   - a negative errno-style value meaning the call failed with nothing sent.
///
/// This mirrors sendmmsg(2), which returns the number of messages sent and only
/// reports an error (-1) when *no* datagram could be sent. Returns the total
/// number of messages sent (0..total). If the very first call fails before any
/// message is sent, the negative error code is propagated unchanged so the
/// caller can inspect errno exactly as it would for a scalar sendto.
template <typename TransmitFn>
int DriveBatchedSend(unsigned total, TransmitFn transmit)
{
	unsigned sent = 0;
	while (sent < total)
	{
		int r = transmit(sent, total - sent);
		if (r > 0)
		{
			sent += (unsigned) r; // accepted r messages; retry the remainder
			continue;
		}
		if (r == 0)
			break; // no progress possible right now; don't spin
		// r < 0: this call failed with nothing sent. Propagate the error only
		// if we have not managed to send anything yet (mirrors sendmmsg, which
		// reports -1 solely when no datagram at all could be sent); otherwise
		// report the progress already made.
		if (sent == 0)
			return r;
		break;
	}
	return (int) sent;
}

/// Fan a received batch out to the event handler.
///
/// \a slots[0..allocated) are pre-allocated recv structs whose .data buffers
/// already hold the received bytes (the mmsghdr iovecs pointed at them). For
/// each i in [0,received) with lens[i] > 0 the struct is stamped with the byte
/// count, source address (from addrs[i]), timestamp \a now and \a socket, then
/// handed to handler->OnRNS2Recv. Every other struct -- short reads
/// (lens[i] <= 0) and the unused tail [received,allocated) -- is returned via
/// handler->DeallocRNS2RecvStruct so no buffer leaks.
void DispatchRecvBatch(RNS2EventHandler *handler,
                       RNS2RecvStruct **slots, unsigned allocated,
                       const int *lens, const sockaddr_storage *addrs,
                       unsigned received, RakNetSocket2 *socket,
                       MafiaNet::TimeUS now);

#if defined(MAFIANET_USE_SENDMMSG)
/// Accumulates already-prepared datagrams (post-encryption, post packet-loss
/// simulation) destined for a single peer and flushes them via
/// RakNetSocket2::SendBatch -- one sendmmsg per burst on Linux.
///
/// Bytes are copied in because the caller's serialization buffer is reused for
/// the next datagram. It is meant to be loop-local: construct it before a send
/// loop, Flush() at the loop's single exit (the destructor also flushes as a
/// backstop), so a datagram is never stranded across ticks.
class RNS2SendBatch
{
public:
	RNS2SendBatch(RakNetSocket2 *socket, const SystemAddress &dest)
		: socket(socket), dest(dest), count(0) {}
	~RNS2SendBatch() { Flush(); }

	/// Append one already-prepared datagram. Oversized datagrams are rejected
	/// rather than truncated: the scalar send path never truncates, and silently
	/// clamping would ship corrupted bytes with no error signal. Callers already
	/// keep datagrams within the MTU (RakAssert in ReliabilityLayer), so this is
	/// a release-build backstop, not the normal path.
	void Add(const char *data, int length)
	{
		RakAssert(length >= 0 && length <= MAXIMUM_MTU_SIZE);
		if (length < 0 || length > MAXIMUM_MTU_SIZE)
			return; // drop, don't corrupt; the reliability layer will resend
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
		socket->SendBatch(sends, count, __FILE__, __LINE__);
		count = 0;
	}

private:
	// One buffer block per update thread, reused across every connection and
	// tick -- the batch is filled and flushed synchronously within a single
	// UpdateInternal call, so there is no reentrancy. This keeps the send path
	// allocation-free (the old per-tick new[] churned ~95 KB each call).
	static char *Slot(unsigned i)
	{
		static thread_local char buffers[MMSG_BATCH_MAX][MAXIMUM_MTU_SIZE];
		return buffers[i];
	}

	RakNetSocket2 *socket;
	SystemAddress dest;
	unsigned count;
	int lengths[MMSG_BATCH_MAX];
};
#endif // MAFIANET_USE_SENDMMSG

} // namespace MafiaNet

#endif // __MAFIANET_MMSG_BATCH_H
