/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hermetic unit tests for the portable batched-datagram helpers in
 *  MmsgBatch.h. These exercise the logic that is genuinely bug-prone in a
 *  recvmmsg/sendmmsg integration -- the sendmmsg partial-send resume loop, the
 *  byte-order handling when decoding a raw sockaddr, and fanning a received
 *  batch out to the event handler -- without touching any actual syscall, so
 *  they run everywhere including platforms that lack recvmmsg/sendmmsg.
 */

#include <gtest/gtest.h>

#include "mafianet/MmsgBatch.h"
#include "mafianet/socket2.h"
#include "mafianet/types.h"

#ifdef _WIN32
#include <ws2tcpip.h> // inet_pton, htons (winsock2 pulled in via socket2.h)
#else
#include <arpa/inet.h> // inet_pton, htons
#endif
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
	sockaddr_storage MakeV4(const char *ip, unsigned short port)
	{
		sockaddr_storage ss;
		memset(&ss, 0, sizeof(ss));
		sockaddr_in *in = reinterpret_cast<sockaddr_in *>(&ss);
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		inet_pton(AF_INET, ip, &in->sin_addr);
		return ss;
	}

	// Records everything DispatchRecvBatch does so tests can assert on it. No
	// allocation happens here -- the tests own the RNS2RecvStruct storage.
	struct RecordingHandler : RNS2EventHandler
	{
		struct Received
		{
			int bytesRead;
			unsigned short port;
			RakNetSocket2 *socket;
			MafiaNet::TimeUS timeRead;
		};

		std::vector<Received> received;
		std::vector<RNS2RecvStruct *> deallocated;

		void OnRNS2Recv(RNS2RecvStruct *s) override
		{
			received.push_back({s->bytesRead, s->systemAddress.GetPort(),
			                    s->socket, s->timeRead});
		}
		void DeallocRNS2RecvStruct(RNS2RecvStruct *s, const char *, unsigned int) override
		{
			deallocated.push_back(s);
		}
		RNS2RecvStruct *AllocRNS2RecvStruct(const char *, unsigned int) override
		{
			return nullptr; // unused by DispatchRecvBatch
		}
	};

	// A sentinel non-null socket pointer; DispatchRecvBatch only stores it.
	RakNetSocket2 *const kSentinelSocket = reinterpret_cast<RakNetSocket2 *>(0xF00D);

	sockaddr_storage MakeFamily(unsigned short family)
	{
		sockaddr_storage ss;
		memset(&ss, 0, sizeof(ss));
		ss.ss_family = family;
		return ss;
	}

#if RAKNET_SUPPORT_IPV6 == 1
	sockaddr_storage MakeV6(const char *ip, unsigned short port)
	{
		sockaddr_storage ss;
		memset(&ss, 0, sizeof(ss));
		sockaddr_in6 *in6 = reinterpret_cast<sockaddr_in6 *>(&ss);
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(port);
		inet_pton(AF_INET6, ip, &in6->sin6_addr);
		return ss;
	}
#endif

	// Records every datagram handed to Send(), so tests can assert on what the
	// batch flushed and when. SendBatch is deliberately NOT overridden: these
	// tests exercise the portable base implementation.
	struct RecordingSocket : RakNetSocket2
	{
		struct Sent
		{
			std::string payload;
			unsigned short port;
			int ttl;
		};

		std::vector<Sent> sent;
		unsigned sendCalls = 0;
		int failFrom = -1; // fail every Send() from this index on
		int failOnly = -1; // fail only the Send() at this index

		RNS2SendResult Send(RNS2_SendParameters *p, const char *, unsigned int) override
		{
			const unsigned index = sendCalls++;
			if (failFrom >= 0 && index >= (unsigned) failFrom)
				return -1;
			if (failOnly >= 0 && index == (unsigned) failOnly)
				return -1;
			sent.push_back({std::string(p->data, (size_t) p->length),
			                p->systemAddress.GetPort(), p->ttl});
			return p->length; // Send() reports bytes, not a datagram count
		}
	};

	SystemAddress MakeDest(unsigned short port)
	{
		SystemAddress a;
		a.FromStringExplicitPort("127.0.0.1", port);
		return a;
	}
}

// ---------------------------------------------------------------------------
// DriveBatchedSend -- the sendmmsg partial-send resume state machine.
// ---------------------------------------------------------------------------

TEST(DriveBatchedSend, SendsWholeBatchInOneCall)
{
	int calls = 0;
	int sent = DriveBatchedSend(5, [&](unsigned offset, unsigned count) {
		++calls;
		EXPECT_EQ(offset, 0u);
		EXPECT_EQ(count, 5u);
		return (int) count;
	});
	EXPECT_EQ(sent, 5);
	EXPECT_EQ(calls, 1);
}

TEST(DriveBatchedSend, ResumesFromOffsetAcrossPartialSends)
{
	// sendmmsg returns short counts; the loop must retry the remainder from the
	// advancing offset until the whole batch is sent.
	std::vector<unsigned> offsets;
	int sent = DriveBatchedSend(7, [&](unsigned offset, unsigned count) {
		offsets.push_back(offset);
		return (int) std::min(count, 3u); // accept at most 3 per call
	});
	EXPECT_EQ(sent, 7);
	EXPECT_EQ(offsets, (std::vector<unsigned>{0u, 3u, 6u}));
}

TEST(DriveBatchedSend, ReturnsErrorWhenNothingCouldBeSent)
{
	// Every message fails permanently, so nothing goes out: report the first
	// error, mirroring sendmmsg's -1-only-when-nothing-was-sent contract.
	int sent = DriveBatchedSend(4, [&](unsigned, unsigned) { return -1; });
	EXPECT_EQ(sent, -1);
}

TEST(DriveBatchedSend, ReturnsTheFirstErrorNotTheLast)
{
	int call = 0;
	int sent = DriveBatchedSend(3, [&](unsigned, unsigned) {
		return (call++ == 0) ? -EMSGSIZE : -EINVAL;
	});
	EXPECT_EQ(sent, -EMSGSIZE);
}

TEST(DriveBatchedSend, SkipsAPermanentlyFailedMessageAndSendsTheRest)
{
	// A negative return means the message at `offset` itself is undeliverable.
	// Stopping there would silently discard every datagram after it -- the
	// scalar Send() loop would have delivered those.
	std::vector<unsigned> offsets;
	int sent = DriveBatchedSend(5, [&](unsigned offset, unsigned count) {
		offsets.push_back(offset);
		if (offset == 2)
			return -EMSGSIZE; // the third datagram is the bad one
		return (int) std::min(count, 1u);
	});
	EXPECT_EQ(sent, 4) << "only the bad datagram should be lost";
	EXPECT_EQ(offsets, (std::vector<unsigned>{0u, 1u, 2u, 3u, 4u}));
}

TEST(DriveBatchedSend, ReturnsProgressWhenEveryRemainingMessageFails)
{
	// First call sends 4, the rest are all undeliverable: we already made
	// progress, so report the 4 that went out rather than the error.
	int call = 0;
	int sent = DriveBatchedSend(9, [&](unsigned, unsigned) {
		return (call++ == 0) ? 4 : -1;
	});
	EXPECT_EQ(sent, 4);
}

TEST(DriveBatchedSend, StopsOnATransientFailureInsteadOfSkippingMessages)
{
	// A transient socket-wide condition (EAGAIN) is reported as 0, not as a
	// negative: the messages are fine, so they must not be dropped one by one.
	int calls = 0;
	int sent = DriveBatchedSend(5, [&](unsigned, unsigned) {
		++calls;
		return calls == 1 ? 2 : 0;
	});
	EXPECT_EQ(sent, 2);
	EXPECT_EQ(calls, 2) << "must not burn a call per remaining datagram";
}

TEST(DriveBatchedSend, StopsOnNoProgressWithoutLooping)
{
	// A zero return means "no progress possible right now" -- stop instead of
	// spinning forever.
	int calls = 0;
	int sent = DriveBatchedSend(6, [&](unsigned, unsigned) {
		++calls;
		return 0;
	});
	EXPECT_EQ(sent, 0);
	EXPECT_EQ(calls, 1);
}

TEST(DriveBatchedSend, EmptyBatchNeverCallsTransmit)
{
	int calls = 0;
	int sent = DriveBatchedSend(0, [&](unsigned, unsigned) {
		++calls;
		return 0;
	});
	EXPECT_EQ(sent, 0);
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// SockaddrToSystemAddress -- byte-order handling.
// ---------------------------------------------------------------------------

TEST(SockaddrToSystemAddress, DecodesIPv4PortToHostOrder)
{
	sockaddr_storage ss = MakeV4("192.0.2.1", 4660);
	SystemAddress out;
	SockaddrToSystemAddress(ss, &out);
	EXPECT_EQ(out.GetPort(), 4660);
	EXPECT_EQ(out.debugPort, 4660);
	EXPECT_EQ(out.GetIPVersion(), 4);
}

TEST(SockaddrToSystemAddress, PreservesIPv4Address)
{
	sockaddr_storage ss = MakeV4("203.0.113.7", 1234);
	const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(&ss);
	SystemAddress out;
	SockaddrToSystemAddress(ss, &out);
	EXPECT_EQ(out.address.addr4.sin_addr.s_addr, in->sin_addr.s_addr);
}

#if RAKNET_SUPPORT_IPV6 == 1
TEST(SockaddrToSystemAddress, DecodesIPv6PortToHostOrder)
{
	sockaddr_storage ss = MakeV6("2001:db8::1", 4660);
	const sockaddr_in6 *in6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
	SystemAddress out;
	ASSERT_TRUE(SockaddrToSystemAddress(ss, &out));
	EXPECT_EQ(out.GetPort(), 4660);
	EXPECT_EQ(out.debugPort, 4660);
	EXPECT_EQ(out.GetIPVersion(), 6);
	EXPECT_EQ(memcmp(&out.address.addr6.sin6_addr, &in6->sin6_addr,
	                 sizeof(in6->sin6_addr)),
	          0);
}
#else
TEST(SockaddrToSystemAddress, RejectsIPv6SourceOnIPv4OnlyBuild)
{
	// An IPv6 sender reaching an IPv4-only build has no representable address;
	// the caller must be told so rather than handed a half-decoded one.
	SystemAddress out;
	EXPECT_FALSE(SockaddrToSystemAddress(MakeFamily(AF_INET6), &out));
	EXPECT_EQ(out, UNASSIGNED_SYSTEM_ADDRESS);
}
#endif

TEST(SockaddrToSystemAddress, RejectsUndecodableFamilyWithoutKeepingStaleAddress)
{
	// recv structs are recycled, so `out` arrives holding the previous sender.
	// An address family this build cannot decode must not leave that in place --
	// the datagram would be attributed to the wrong peer.
	SystemAddress out;
	ASSERT_TRUE(SockaddrToSystemAddress(MakeV4("198.51.100.9", 5555), &out));
	ASSERT_EQ(out.GetPort(), 5555);

	sockaddr_storage unknown = MakeFamily(AF_UNSPEC);
	EXPECT_FALSE(SockaddrToSystemAddress(unknown, &out));
	EXPECT_EQ(out, UNASSIGNED_SYSTEM_ADDRESS);
}

// ---------------------------------------------------------------------------
// DispatchRecvBatch -- fanning a received batch out to the handler.
// ---------------------------------------------------------------------------

TEST(DispatchRecvBatch, DispatchesEachReceivedDatagramAndFreesTheTail)
{
	const unsigned allocated = 4;
	std::vector<RNS2RecvStruct> storage(allocated);
	RNS2RecvStruct *slots[allocated];
	for (unsigned i = 0; i < allocated; ++i)
		slots[i] = &storage[i];

	int lens[allocated] = {11, 22, 0, 0};
	sockaddr_storage addrs[allocated] = {
	    MakeV4("10.0.0.1", 1111), MakeV4("10.0.0.2", 2222), {}, {}};

	RecordingHandler handler;
	DispatchRecvBatch(&handler, slots, allocated, lens, addrs,
	                  /*received=*/2, kSentinelSocket, /*now=*/12345);

	ASSERT_EQ(handler.received.size(), 2u);
	EXPECT_EQ(handler.received[0].bytesRead, 11);
	EXPECT_EQ(handler.received[0].port, 1111);
	EXPECT_EQ(handler.received[0].socket, kSentinelSocket);
	EXPECT_EQ(handler.received[0].timeRead, 12345u);
	EXPECT_EQ(handler.received[1].bytesRead, 22);
	EXPECT_EQ(handler.received[1].port, 2222);

	// The two unused tail slots must be handed back, not leaked.
	EXPECT_EQ(handler.deallocated.size(), 2u);
}

TEST(DispatchRecvBatch, FreesZeroLengthDatagramsInsteadOfDispatching)
{
	// A zero-length read mirrors the scalar path's bytesRead<=0 branch: free it,
	// don't surface it as a packet.
	const unsigned allocated = 3;
	std::vector<RNS2RecvStruct> storage(allocated);
	RNS2RecvStruct *slots[allocated];
	for (unsigned i = 0; i < allocated; ++i)
		slots[i] = &storage[i];

	int lens[allocated] = {0, 22, 0};
	sockaddr_storage addrs[allocated] = {
	    MakeV4("10.0.0.1", 1111), MakeV4("10.0.0.2", 2222), {}};

	RecordingHandler handler;
	DispatchRecvBatch(&handler, slots, allocated, lens, addrs,
	                  /*received=*/2, kSentinelSocket, /*now=*/1);

	ASSERT_EQ(handler.received.size(), 1u);
	EXPECT_EQ(handler.received[0].bytesRead, 22);
	EXPECT_EQ(handler.received[0].port, 2222);
	// Freed: the zero-length slot 0 and the unused tail slot 2.
	EXPECT_EQ(handler.deallocated.size(), 2u);
}

TEST(DispatchRecvBatch, FreesEverythingWhenNothingReceived)
{
	const unsigned allocated = 3;
	std::vector<RNS2RecvStruct> storage(allocated);
	RNS2RecvStruct *slots[allocated];
	for (unsigned i = 0; i < allocated; ++i)
		slots[i] = &storage[i];

	int lens[allocated] = {0, 0, 0};
	sockaddr_storage addrs[allocated] = {{}, {}, {}};

	RecordingHandler handler;
	DispatchRecvBatch(&handler, slots, allocated, lens, addrs,
	                  /*received=*/0, kSentinelSocket, /*now=*/1);

	EXPECT_TRUE(handler.received.empty());
	EXPECT_EQ(handler.deallocated.size(), 3u);
}

TEST(DispatchRecvBatch, FreesDatagramsWhoseSourceAddressCannotBeDecoded)
{
	const unsigned allocated = 2;
	std::vector<RNS2RecvStruct> storage(allocated);
	RNS2RecvStruct *slots[allocated];
	for (unsigned i = 0; i < allocated; ++i)
		slots[i] = &storage[i];

	int lens[allocated] = {11, 22};
	sockaddr_storage addrs[allocated] = {MakeFamily(AF_UNSPEC),
	                                     MakeV4("10.0.0.2", 2222)};

	RecordingHandler handler;
	DispatchRecvBatch(&handler, slots, allocated, lens, addrs,
	                  /*received=*/2, kSentinelSocket, /*now=*/1);

	// Slot 0 has bytes but no usable source address: free it rather than
	// surfacing a packet attributed to the recycled struct's previous sender.
	ASSERT_EQ(handler.received.size(), 1u);
	EXPECT_EQ(handler.received[0].bytesRead, 22);
	EXPECT_EQ(handler.received[0].port, 2222);
	ASSERT_EQ(handler.deallocated.size(), 1u);
	EXPECT_EQ(handler.deallocated[0], slots[0]);
}

TEST(DispatchRecvBatch, ClampsAReceivedCountLargerThanTheAllocation)
{
	// Defensive clamp: a received count above the number of slots would otherwise
	// walk off the end of the arrays. Nothing beyond the allocation is touched.
	const unsigned allocated = 2;
	std::vector<RNS2RecvStruct> storage(allocated);
	RNS2RecvStruct *slots[allocated];
	for (unsigned i = 0; i < allocated; ++i)
		slots[i] = &storage[i];

	int lens[allocated] = {11, 22};
	sockaddr_storage addrs[allocated] = {MakeV4("10.0.0.1", 1111),
	                                     MakeV4("10.0.0.2", 2222)};

	RecordingHandler handler;
	DispatchRecvBatch(&handler, slots, allocated, lens, addrs,
	                  /*received=*/9, kSentinelSocket, /*now=*/1);

	ASSERT_EQ(handler.received.size(), 2u);
	EXPECT_EQ(handler.received[0].port, 1111);
	EXPECT_EQ(handler.received[1].port, 2222);
	EXPECT_TRUE(handler.deallocated.empty());
}

// ---------------------------------------------------------------------------
// RakNetSocket2::SendBatch -- the portable default must match the sendmmsg
// override's contract: a datagram COUNT, and an error only if nothing went out.
// ---------------------------------------------------------------------------

TEST(SocketSendBatch, ReturnsDatagramCountNotByteTotal)
{
	RecordingSocket socket;
	RNS2_SendParameters sends[3];
	char payload[64] = {};
	for (unsigned i = 0; i < 3; ++i)
	{
		sends[i].data = payload;
		sends[i].length = 40; // 3 * 40 bytes, but only 3 datagrams
		sends[i].systemAddress = MakeDest(1000);
	}

	EXPECT_EQ(socket.SendBatch(sends, 3, _FILE_AND_LINE_), 3);
	EXPECT_EQ(socket.sent.size(), 3u);
}

TEST(SocketSendBatch, PropagatesErrorOnlyWhenNothingWasSent)
{
	RNS2_SendParameters sends[4];
	char payload[8] = {};
	for (unsigned i = 0; i < 4; ++i)
	{
		sends[i].data = payload;
		sends[i].length = 8;
		sends[i].systemAddress = MakeDest(1000);
	}

	RecordingSocket allFail;
	allFail.failFrom = 0;
	EXPECT_EQ(allFail.SendBatch(sends, 4, _FILE_AND_LINE_), -1);

	// Two out, then a failure: report the progress made, like sendmmsg does.
	RecordingSocket partial;
	partial.failFrom = 2;
	EXPECT_EQ(partial.SendBatch(sends, 4, _FILE_AND_LINE_), 2);
	EXPECT_EQ(partial.sent.size(), 2u);
}

TEST(SocketSendBatch, DropsOnlyTheDatagramThatFailed)
{
	// One bad destination in the middle must not take the rest of the batch with
	// it -- the plain Send() loop this replaced kept going after a failed sendto.
	RecordingSocket socket;
	socket.failOnly = 1;
	RNS2_SendParameters sends[4];
	char payload[4] = {};
	for (unsigned i = 0; i < 4; ++i)
	{
		sends[i].data = payload;
		sends[i].length = 4;
		sends[i].systemAddress = MakeDest((unsigned short) (1000 + i));
	}

	EXPECT_EQ(socket.SendBatch(sends, 4, _FILE_AND_LINE_), 3);
	ASSERT_EQ(socket.sent.size(), 3u);
	EXPECT_EQ(socket.sent[0].port, 1000);
	EXPECT_EQ(socket.sent[1].port, 1002) << "the datagram after the bad one must still go out";
	EXPECT_EQ(socket.sent[2].port, 1003);
}

TEST(SocketSendBatch, ForwardsPerDatagramTtl)
{
	// sendmmsg has no per-message TTL, so RNS2_Linux::SendBatch defers a batch
	// carrying one to this base loop. That only preserves the TTL if the loop
	// hands the whole parameter through to Send() untouched.
	RecordingSocket socket;
	RNS2_SendParameters sends[2];
	char payload[4] = {};
	for (unsigned i = 0; i < 2; ++i)
	{
		sends[i].data = payload;
		sends[i].length = 4;
		sends[i].systemAddress = MakeDest(1000);
		sends[i].ttl = (int) (i + 1);
	}

	EXPECT_EQ(socket.SendBatch(sends, 2, _FILE_AND_LINE_), 2);
	ASSERT_EQ(socket.sent.size(), 2u);
	EXPECT_EQ(socket.sent[0].ttl, 1);
	EXPECT_EQ(socket.sent[1].ttl, 2);
}

// ---------------------------------------------------------------------------
// RNS2SendBatch -- accumulating datagrams and flushing them through SendBatch.
// ---------------------------------------------------------------------------

TEST(RNS2SendBatch, FlushesWithoutATtlSoTheSendmmsgPathIsNeverDeferred)
{
	// The reliability layer never asks for a TTL; keeping it zero is what lets
	// RNS2_Linux::SendBatch take the real sendmmsg path on every flush.
	RecordingSocket socket;
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		batch.Add("x", 1);
	}
	ASSERT_EQ(socket.sent.size(), 1u);
	EXPECT_EQ(socket.sent[0].ttl, 0);
}

TEST(RNS2SendBatch, CopiesPayloadsSoCallerCanReuseItsBuffer)
{
	// The reliability layer reuses one serialization buffer per datagram, so the
	// batch must copy rather than alias.
	RecordingSocket socket;
	char scratch[8];
	{
		RNS2SendBatch batch(&socket, MakeDest(4242));
		memcpy(scratch, "first", 5);
		batch.Add(scratch, 5);
		memcpy(scratch, "secnd", 5);
		batch.Add(scratch, 5);
		EXPECT_TRUE(socket.sent.empty()) << "nothing should go out before Flush";
	}

	ASSERT_EQ(socket.sent.size(), 2u);
	EXPECT_EQ(socket.sent[0].payload, "first");
	EXPECT_EQ(socket.sent[1].payload, "secnd");
	EXPECT_EQ(socket.sent[0].port, 4242);
	EXPECT_EQ(socket.sent[1].port, 4242);
}

TEST(RNS2SendBatch, DestructorFlushesAsBackstop)
{
	RecordingSocket socket;
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		batch.Add("x", 1);
	}
	ASSERT_EQ(socket.sent.size(), 1u);
	EXPECT_EQ(socket.sent[0].payload, "x");
}

TEST(RNS2SendBatch, ExplicitFlushIsNotRepeatedByTheDestructor)
{
	RecordingSocket socket;
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		batch.Add("x", 1);
		batch.Flush();
		EXPECT_EQ(socket.sent.size(), 1u);
	}
	EXPECT_EQ(socket.sent.size(), 1u) << "destructor must not re-send a flushed batch";
}

TEST(RNS2SendBatch, FlushesAtTheBatchBoundaryInsteadOfOverrunning)
{
	RecordingSocket socket;
	{
		RNS2SendBatch batch(&socket, MakeDest(7000));
		for (unsigned i = 0; i < MMSG_BATCH_MAX + 3; ++i)
		{
			const char byte = (char) ('a' + (i % 26));
			batch.Add(&byte, 1);
		}
		// The first MMSG_BATCH_MAX are flushed when slot MMSG_BATCH_MAX is added.
		EXPECT_EQ(socket.sent.size(), (size_t) MMSG_BATCH_MAX);
	}

	ASSERT_EQ(socket.sent.size(), (size_t) MMSG_BATCH_MAX + 3);
	for (unsigned i = 0; i < MMSG_BATCH_MAX + 3; ++i)
		EXPECT_EQ(socket.sent[i].payload, std::string(1, (char) ('a' + (i % 26))))
		    << "datagram " << i << " out of order or corrupted across the boundary";
}

TEST(RNS2SendBatch, DropsOversizedDatagramsInsteadOfTruncating)
{
	// Silently clamping would ship corrupted bytes with no error signal; the
	// reliability layer resends what never went out.
#if defined(_DEBUG)
	GTEST_SKIP() << "the oversized case trips RakAssert by design in debug builds";
#else
	RecordingSocket socket;
	std::vector<char> oversized(MAXIMUM_MTU_SIZE + 1, 'z');
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		batch.Add(oversized.data(), MAXIMUM_MTU_SIZE + 1);
		batch.Add("ok", 2);
	}
	ASSERT_EQ(socket.sent.size(), 1u);
	EXPECT_EQ(socket.sent[0].payload, "ok");
#endif
}

TEST(RNS2SendBatch, FailedFlushDropsTheBatchInsteadOfRetryingIt)
{
	// Flush clears the batch unconditionally. Carrying refused datagrams into
	// the next flush would resend them behind whatever was queued meanwhile,
	// reordering the stream; the reliability layer already handles the resend.
	RecordingSocket socket;
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		socket.failFrom = 0;
		batch.Add("dropped", 7);
		batch.Flush();
		EXPECT_TRUE(socket.sent.empty());

		socket.failFrom = -1;
		batch.Add("fresh", 5);
	}
	ASSERT_EQ(socket.sent.size(), 1u) << "the refused datagram must not be resent";
	EXPECT_EQ(socket.sent[0].payload, "fresh");
}

TEST(RNS2SendBatch, PartiallyFailedFlushStillDeliversTheRest)
{
	RecordingSocket socket;
	socket.failOnly = 1;
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		batch.Add("aa", 2);
		batch.Add("bb", 2);
		batch.Add("cc", 2);
	}
	ASSERT_EQ(socket.sent.size(), 2u);
	EXPECT_EQ(socket.sent[0].payload, "aa");
	EXPECT_EQ(socket.sent[1].payload, "cc");
}

TEST(RNS2SendBatch, EmptyBatchSendsNothing)
{
	RecordingSocket socket;
	{
		RNS2SendBatch batch(&socket, MakeDest(1234));
		(void) batch;
	}
	EXPECT_EQ(socket.sendCalls, 0u);
}
