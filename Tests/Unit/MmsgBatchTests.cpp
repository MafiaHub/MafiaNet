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
#include <cstring>
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

TEST(DriveBatchedSend, ReturnsErrorWhenFirstCallSendsNothing)
{
	// sendmmsg only reports -1 when NO datagram could be sent; propagate it so
	// the caller sees errno exactly as for a scalar sendto.
	int sent = DriveBatchedSend(4, [&](unsigned, unsigned) { return -1; });
	EXPECT_EQ(sent, -1);
}

TEST(DriveBatchedSend, ReturnsProgressWhenErrorFollowsPartialSend)
{
	// First call sends 4, second fails: we already made progress, so report the
	// 4 that went out rather than the error.
	int call = 0;
	int sent = DriveBatchedSend(9, [&](unsigned, unsigned) {
		return (call++ == 0) ? 4 : -1;
	});
	EXPECT_EQ(sent, 4);
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
