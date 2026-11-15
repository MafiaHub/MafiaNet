/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Two real ReliabilityLayer instances joined by a fake socket, with fully
 *  simulated time -- no RakPeer, no loopback traffic, no threads, no wall
 *  clock. Each layer writes its datagrams into a CapturingSocket, and the test
 *  decides which of them reach the other layer, in which order, and how often.
 *
 *  Two flavours share the socket:
 *  - MafiaNetTest::ReliabilityLayerPair, a plain struct a test (or a helper
 *    struct) holds or derives from and drives step by step. Used by the
 *    characterization and split-packet tests.
 *  - RelPairTest::ReliabilityLayerPair, a gtest fixture whose channel applies
 *    a per-test drop rule (to model a tunnel or plain loss), logs every a->b
 *    datagram, and lets a test inject extra datagrams towards b. Shared by
 *    ReliabilityLayerBlackHoleTests.cpp, ReliabilityLayerCryptoTests.cpp and
 *    ReliabilityLayerReSplitHeaderTests.cpp.
 *
 *  `now` starts at the real clock: ReliabilityLayer stamps
 *  timeLastDatagramArrived with the real GetTimeMS() on every receive, and
 *  AckTimeout compares that against the time passed in. A fixed fake epoch
 *  fails on any machine whose process uptime exceeds it (this killed every
 *  test on Windows CI). All progress is still driven by fixed fake increments
 *  from this anchor.
 */

#ifndef MAFIANET_TESTS_RELIABILITY_LAYER_PAIR_H
#define MAFIANET_TESTS_RELIABILITY_LAYER_PAIR_H

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/mtu_size.h"
#include "mafianet/packet_priority.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/rand.h"
#include "mafianet/reliability_layer.h"
#include "mafianet/session_crypto.h"
#include "mafianet/socket2.h"

namespace MafiaNetTest
{

// Captures every datagram a ReliabilityLayer hands to the OS, so the test can
// shuttle it to the other endpoint (or drop, reorder or duplicate it).
struct CapturingSocket : public MafiaNet::RakNetSocket2
{
	std::vector<std::vector<char>> sent;

	MafiaNet::RNS2SendResult Send(MafiaNet::RNS2_SendParameters *sendParameters) override
	{
		sent.push_back(std::vector<char>(sendParameters->data, sendParameters->data + sendParameters->length));
		return sendParameters->length;
	}
};

// Both layers run in mode Plaintext: the keyless cleartext path. A layer in
// mode Required sends and accepts nothing until it has session keys.
struct ReliabilityLayerPair
{
	// Large enough that no layer ever times out within a test. The default
	// (10 s, 30 s in Debug) is measured from the real clock at Reset() while
	// `now` runs ahead in 10 ms steps: a sender whose unacked messages outlive
	// it would go dead and stop sending.
	static constexpr MafiaNet::TimeMS kNeverTimeOutMs = 1000000;

	MafiaNet::ReliabilityLayer a, b;
	CapturingSocket aSock, bSock;
	MafiaNet::SystemAddress aAddr, bAddr;
	std::vector<MafiaNet::PluginInterface2 *> handlers;
	MafiaNet::RakNetRandom rnr;
	MafiaNet::BitStream updateBitStream{MAXIMUM_MTU_SIZE};
	CCTimeType now = MafiaNet::GetTimeUS();

	ReliabilityLayerPair()
	{
		EXPECT_TRUE(aAddr.FromStringExplicitPort("127.0.0.1", 40001));
		EXPECT_TRUE(bAddr.FromStringExplicitPort("127.0.0.1", 40002));
		ResetA();
		ResetB();
	}

	void ResetA()
	{
		a.SetEncryptionMode(MafiaNet::EncryptionMode::Plaintext);
		a.Reset(true, MAXIMUM_MTU_SIZE);
		a.SetTimeoutTime(kNeverTimeOutMs);
		ReanchorClock();
	}
	void ResetB()
	{
		b.SetEncryptionMode(MafiaNet::EncryptionMode::Plaintext);
		b.Reset(true, MAXIMUM_MTU_SIZE);
		b.SetTimeoutTime(kNeverTimeOutMs);
		ReanchorClock();
	}

	// Reset() stamps the layer's lastUpdateTime with the real clock, and Update()
	// does nothing for a time at or before it: keep `now` from falling behind.
	void ReanchorClock()
	{
		const CCTimeType real = MafiaNet::GetTimeUS();
		if (real > now)
			now = real;
	}

	void UpdateA() { a.Update(&aSock, bAddr, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream); }
	void UpdateB() { b.Update(&bSock, aAddr, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream); }

	// Hands one datagram a sent to b, and one b sent to a.
	void DeliverToB(std::vector<char> &datagram)
	{
		b.HandleSocketReceiveFromConnectedPlayer(datagram.data(), (unsigned int) datagram.size(), aAddr, handlers,
		                                         MAXIMUM_MTU_SIZE, &bSock, &rnr, now, updateBitStream);
	}
	void DeliverToA(std::vector<char> &datagram)
	{
		a.HandleSocketReceiveFromConnectedPlayer(datagram.data(), (unsigned int) datagram.size(), bAddr, handlers,
		                                         MAXIMUM_MTU_SIZE, &aSock, &rnr, now, updateBitStream);
	}

	// Every complete message `layer` has ready, in delivery order.
	static std::vector<std::vector<unsigned char>> Drain(MafiaNet::ReliabilityLayer &layer)
	{
		std::vector<std::vector<unsigned char>> messages;
		unsigned char *data;
		MafiaNet::BitSize_t bits;
		while ((bits = layer.Receive(&data)) > 0)
		{
			messages.emplace_back(data, data + BITS_TO_BYTES(bits));
			std::free(data);
		}
		return messages;
	}
};

} // namespace MafiaNetTest

namespace RelPairTest
{

const int TEST_MTU = MAXIMUM_MTU_SIZE;
const MafiaNet::TimeMS TEST_TIMEOUT_MS = 1000000;

using MafiaNetTest::CapturingSocket;

class ReliabilityLayerPair : public ::testing::Test
{
protected:
	MafiaNet::ReliabilityLayer a, b;
	CapturingSocket aSock, bSock;
	MafiaNet::SystemAddress aAddr, bAddr;
	std::vector<MafiaNet::PluginInterface2 *> handlers;
	MafiaNet::RakNetRandom rnr;
	MafiaNet::BitStream updateBitStream;
	CCTimeType now;

	// Datagrams from a to b strictly larger than this many socket-level bytes
	// are dropped, like a tunnel whose ceiling the handshake never probed.
	// Socket-level bytes exclude the UDP/IP headers the MTU figures include.
	int aToBDropOverBytes;
	// When non-zero, drop this fraction (in percent) of ALL a->b datagrams,
	// deterministically, to model plain loss.
	int aToBLossPercent;
	uint32_t lcgState;
	// The same for b->a datagrams (acks, mostly), from a stream of its own.
	int bToALossPercent;
	uint32_t bToALcgState;

	// Every datagram a handed to its socket, delivered or dropped, with a's
	// current MTU at the time it was built.
	struct LoggedDatagram
	{
		std::vector<char> bytes;
		int mtuAtSend;
	};
	std::vector<LoggedDatagram> aToBLog;

	// When set, also drop every a->b datagram for which this returns true.
	std::function<bool(const std::vector<char> &)> dropAToBIf;

	// Delivered to b on the next Tick, after that tick's real a->b traffic.
	std::vector<std::vector<char>> injectToB;

	std::vector<std::vector<unsigned char>> received; // complete messages b got
	std::vector<MafiaNet::BitSize_t> receivedBits;    // their exact bit lengths

	ReliabilityLayerPair()
	    : updateBitStream(MAXIMUM_MTU_SIZE), now(0), aToBDropOverBytes(MAXIMUM_MTU_SIZE), aToBLossPercent(0), lcgState(0x12345678), bToALossPercent(0), bToALcgState(0x9E3779B9)
	{
	}

	virtual void SetUp()
	{
		// The simulated clock must start at the real clock: ReliabilityLayer
		// stamps timeLastDatagramArrived with the real GetTimeMS() on every
		// receive, and AckTimeout compares that against the time we pass in.
		// A fixed fake epoch fails on any machine whose process uptime exceeds
		// it (this killed every test on Windows CI). All progress is still
		// driven by fixed fake increments from this anchor.
		now = MafiaNet::GetTimeUS();

		ASSERT_TRUE(aAddr.FromStringExplicitPort("127.0.0.1", 40001));
		ASSERT_TRUE(bAddr.FromStringExplicitPort("127.0.0.1", 40002));
		// Mode Plaintext: the keyless cleartext path. A layer in mode Required
		// sends and accepts nothing until InstallSessionKeys().
		ResetLayers(MafiaNet::EncryptionMode::Plaintext);
	}

	// Resets both layers for a fresh connection in \a mode. Wipes any keys.
	void ResetLayers(MafiaNet::EncryptionMode mode)
	{
		a.SetEncryptionMode(mode);
		b.SetEncryptionMode(mode);
		a.Reset(true, TEST_MTU);
		b.Reset(true, TEST_MTU);
		a.SetTimeoutTime(TEST_TIMEOUT_MS);
		b.SetTimeoutTime(TEST_TIMEOUT_MS);
		ReanchorClock();
	}

	// Reset() stamps the layer's lastUpdateTime with the real clock, and Update()
	// does nothing for a time at or before it. Call this after any Reset() so the
	// fake clock is never behind a layer: otherwise, on a loaded machine, the next
	// Update(now + small step) can silently return without sending.
	void ReanchorClock()
	{
		const CCTimeType real = MafiaNet::GetTimeUS();
		if (real > now)
			now = real;
	}

	// Runs a real X25519 exchange (a initiates) and installs the resulting
	// directional keys in both layers, as the connection handshake will.
	// The layers must be in mode Required (ResetLayers).
	void InstallSessionKeys()
	{
		const MafiaNet::RakNetGUID aGuid(0x1111111111111111ull);
		const MafiaNet::RakNetGUID bGuid(0x2222222222222222ull);
		MafiaNet::EphemeralKeyPair aPair;
		MafiaNet::EphemeralKeyPair bPair;
		const MafiaNet::CryptInfo aInfo = aPair.MakeCryptInfo();
		const MafiaNet::CryptInfo bInfo = bPair.MakeCryptInfo();
		MafiaNet::SessionKeys aKeys;
		MafiaNet::SessionKeys bKeys;
		ASSERT_TRUE(MafiaNet::DeriveSessionKeys(aPair, true, aInfo, bInfo, aGuid, bGuid, &aKeys));
		ASSERT_TRUE(MafiaNet::DeriveSessionKeys(bPair, false, aInfo, bInfo, aGuid, bGuid, &bKeys));
		ASSERT_TRUE(a.SetSessionKeys(aKeys));
		ASSERT_TRUE(b.SetSessionKeys(bKeys));
	}

	bool DropAToB(int datagramBytes)
	{
		if (datagramBytes > aToBDropOverBytes)
			return true;
		if (aToBLossPercent > 0)
		{
			lcgState = lcgState * 1664525u + 1013904223u;
			if ((int) (lcgState % 100u) < aToBLossPercent)
				return true;
		}
		return false;
	}

	bool DropBToA()
	{
		if (bToALossPercent == 0)
			return false;
		bToALcgState = bToALcgState * 1664525u + 1013904223u;
		return (int) ((bToALcgState >> 8) % 100u) < bToALossPercent;
	}

	void SendFromA(const std::vector<unsigned char> &payload, MafiaNet::Reliability reliability = MafiaNet::Reliability::ReliableOrdered)
	{
		ASSERT_TRUE(a.Send((char *) &payload[0], BYTES_TO_BITS((MafiaNet::BitSize_t) payload.size()),
		                   MafiaNet::Priority::Medium, reliability, 0, true, TEST_MTU, now, 0));
	}

	void DeliverToB(std::vector<char> &datagram)
	{
		b.HandleSocketReceiveFromConnectedPlayer(&datagram[0], (unsigned int) datagram.size(),
		                                         aAddr, handlers, TEST_MTU, &bSock, &rnr, now, updateBitStream);
	}

	// One 10ms tick: update both layers, then deliver the surviving datagrams.
	void Tick()
	{
		now += 10000; // CCTimeType is microseconds
		a.Update(&aSock, bAddr, TEST_MTU, now, 0, handlers, &rnr, updateBitStream);
		b.Update(&bSock, aAddr, TEST_MTU, now, 0, handlers, &rnr, updateBitStream);

		for (size_t i = 0; i < aSock.sent.size(); i++)
		{
			LoggedDatagram logged;
			logged.bytes = aSock.sent[i];
			logged.mtuAtSend = a.GetCurrentMtuBytes();
			aToBLog.push_back(logged);
			if (DropAToB((int) aSock.sent[i].size()))
				continue;
			if (dropAToBIf && dropAToBIf(aSock.sent[i]))
				continue;
			DeliverToB(aSock.sent[i]);
		}
		aSock.sent.clear();

		for (size_t i = 0; i < injectToB.size(); i++)
			DeliverToB(injectToB[i]);
		injectToB.clear();

		for (size_t i = 0; i < bSock.sent.size(); i++)
		{
			// The black hole under test is one-directional: the return path only
			// ever sees plain loss.
			if (DropBToA())
				continue;
			a.HandleSocketReceiveFromConnectedPlayer(&bSock.sent[i][0], (unsigned int) bSock.sent[i].size(),
			                                         bAddr, handlers, TEST_MTU, &aSock, &rnr, now, updateBitStream);
		}
		bSock.sent.clear();

		unsigned char *data;
		MafiaNet::BitSize_t bitSize;
		while ((bitSize = b.Receive(&data)) > 0)
		{
			received.push_back(std::vector<unsigned char>(data, data + BITS_TO_BYTES(bitSize)));
			receivedBits.push_back(bitSize);
			std::free(data);
		}
	}

	// Pump until b has assembled wantMessages complete messages or simulated
	// time runs out. Returns whether the goal was reached.
	bool PumpUntilReceived(size_t wantMessages, int maxSimulatedMs)
	{
		for (int elapsed = 0; elapsed < maxSimulatedMs; elapsed += 10)
		{
			Tick();
			if (received.size() >= wantMessages)
				return true;
			EXPECT_FALSE(a.IsDeadConnection());
			EXPECT_FALSE(b.IsDeadConnection());
			if (a.IsDeadConnection() || b.IsDeadConnection())
				return false;
		}
		return received.size() >= wantMessages;
	}

	// Runs \a ms of simulated time regardless of what arrives.
	void PumpFor(int ms)
	{
		for (int elapsed = 0; elapsed < ms; elapsed += 10)
			Tick();
	}

	static std::vector<unsigned char> PatternMessage(size_t bytes, unsigned char seed)
	{
		std::vector<unsigned char> msg(bytes);
		msg[0] = 200; // stay clear of internal message ids
		for (size_t i = 1; i < bytes; i++)
			msg[i] = (unsigned char) (seed + i * 31);
		return msg;
	}
};

} // namespace RelPairTest

#endif // MAFIANET_TESTS_RELIABILITY_LAYER_PAIR_H
