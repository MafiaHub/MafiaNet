/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hermetic tests for datagram protection in ReliabilityLayer: once session
 *  keys are installed, every datagram is sealed with the session cipher on the
 *  way out and opened (or dropped and counted) on the way in. Two real layers
 *  joined by a fake socket, simulated time (Tests/Support/ReliabilityLayerPair.h).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "ReliabilityLayerPair.h"
#include "mafianet/session_crypto.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;
using namespace RelPairTest;

namespace
{

class ReliabilityLayerCrypto : public ReliabilityLayerPair
{
protected:
	virtual void SetUp()
	{
		ReliabilityLayerPair::SetUp();
		ResetLayers(EncryptionMode::Required);
		InstallSessionKeys();
	}

	uint64_t DroppedUnauthenticatedAtB()
	{
		RakNetStatistics stats;
		b.GetStatistics(&stats);
		return stats.droppedUnauthenticatedDatagrams;
	}

	// True when any 16-byte window of \a payload occurs in \a datagram.
	static bool ContainsAnyWindowOf(const std::vector<char> &datagram, const std::vector<unsigned char> &payload)
	{
		const size_t window = 16;
		for (size_t start = 0; start + window <= payload.size(); start++)
		{
			const char *first = (const char *) &payload[start];
			if (std::search(datagram.begin(), datagram.end(), first, first + window) != datagram.end())
				return true;
		}
		return false;
	}

	size_t LargestLoggedDatagram() const
	{
		size_t largest = 0;
		for (size_t i = 0; i < aToBLog.size(); i++)
			largest = std::max(largest, aToBLog[i].bytes.size());
		return largest;
	}
};

TEST_F(ReliabilityLayerCrypto, DeliversOrderedMessagesWhenEncrypted)
{
	ASSERT_TRUE(a.IsEncrypted());
	ASSERT_TRUE(b.IsEncrypted());

	std::vector<std::vector<unsigned char>> messages;
	messages.push_back(PatternMessage(60, 1));
	messages.push_back(PatternMessage(8000, 2)); // split across many datagrams
	messages.push_back(PatternMessage(1, 3));
	messages.push_back(PatternMessage(700, 4));
	for (size_t i = 0; i < messages.size(); i++)
		SendFromA(messages[i]);

	ASSERT_TRUE(PumpUntilReceived(messages.size(), 30000));
	ASSERT_EQ(messages.size(), received.size());
	for (size_t i = 0; i < messages.size(); i++)
		EXPECT_EQ(messages[i], received[i]) << "message " << i;
	EXPECT_EQ(0u, DroppedUnauthenticatedAtB());
}

TEST_F(ReliabilityLayerCrypto, WireBytesNeverContainPlaintextPayload)
{
	std::vector<unsigned char> msg = PatternMessage(600, 5);
	SendFromA(msg);
	ASSERT_TRUE(PumpUntilReceived(1, 30000));
	EXPECT_EQ(msg, received[0]);

	ASSERT_FALSE(aToBLog.empty());
	for (size_t i = 0; i < aToBLog.size(); i++)
		EXPECT_FALSE(ContainsAnyWindowOf(aToBLog[i].bytes, msg)) << "datagram " << i << " carries the payload in clear";
}

TEST_F(ReliabilityLayerCrypto, ForgedDatagramIsDroppedAndCounted)
{
	std::vector<unsigned char> msg = PatternMessage(3000, 6);
	SendFromA(msg);

	// 100 random datagrams with the connected-datagram bit set, injected while
	// the real transfer is in flight. Fixed-seed LCG: deterministic.
	uint32_t seed = 0xC0FFEEu;
	for (int i = 0; i < 100; i++)
	{
		seed = seed * 1664525u + 1013904223u;
		const size_t len = 20 + (seed >> 8) % 400;
		std::vector<char> forged(len);
		for (size_t j = 0; j < len; j++)
		{
			seed = seed * 1664525u + 1013904223u;
			forged[j] = (char) (seed >> 24);
		}
		forged[0] = (char) (forged[0] | 0x80);
		injectToB.push_back(forged);
		if (i % 10 == 9)
			Tick();
	}

	ASSERT_TRUE(PumpUntilReceived(1, 30000));
	EXPECT_EQ(msg, received[0]);
	EXPECT_EQ(1u, received.size());
	EXPECT_EQ(100u, DroppedUnauthenticatedAtB());
	EXPECT_FALSE(b.IsDeadConnection());
}

TEST_F(ReliabilityLayerCrypto, ReplayedDatagramIsDropped)
{
	// Unreliable, so the reliability layer's own duplicate detection cannot
	// mask a replay: only the cipher's replay window stops a second delivery.
	std::vector<unsigned char> msg = PatternMessage(100, 7);
	SendFromA(msg, Reliability::Unreliable);
	ASSERT_TRUE(PumpUntilReceived(1, 1000));
	ASSERT_FALSE(aToBLog.empty());

	std::vector<char> carrier;
	for (size_t i = 0; i < aToBLog.size() && carrier.empty(); i++)
	{
		if (aToBLog[i].bytes.size() > msg.size())
			carrier = aToBLog[i].bytes;
	}
	ASSERT_FALSE(carrier.empty()) << "no datagram large enough to carry the message";

	const uint64_t droppedBefore = DroppedUnauthenticatedAtB();
	injectToB.push_back(carrier);
	PumpFor(500);

	EXPECT_EQ(1u, received.size()) << "the replayed datagram was delivered again";
	EXPECT_EQ(droppedBefore + 1, DroppedUnauthenticatedAtB());
}

TEST_F(ReliabilityLayerCrypto, SealedDatagramsNeverExceedMtu)
{
	ASSERT_TRUE(a.IsEncrypted());

	std::vector<unsigned char> msg = PatternMessage(20000, 8);
	SendFromA(msg);
	SendFromA(PatternMessage(1400, 9));
	ASSERT_TRUE(PumpUntilReceived(2, 30000));
	EXPECT_EQ(msg, received[0]);

	for (size_t i = 0; i < aToBLog.size(); i++)
		EXPECT_LE((int) aToBLog[i].bytes.size(), TEST_MTU - UDP_HEADER_SIZE) << "datagram " << i;
	// Split fragments fill the budget: the largest sealed datagram lands within
	// the reserved overhead of the limit, so the budget is not counted twice.
	EXPECT_GT((int) LargestLoggedDatagram(), TEST_MTU - UDP_HEADER_SIZE - DatagramCipher::kMaxOverhead);
}

TEST_F(ReliabilityLayerCrypto, SealedDatagramsNeverExceedMtuAfterStepDown)
{
	// As RelLayerBlackHole.RecoversWhenTheForwardPathBlackHolesLargeDatagrams,
	// but sealed: the step-down must budget the cipher overhead too.
	ASSERT_TRUE(a.IsEncrypted());
	aToBDropOverBytes = 1100;

	std::vector<unsigned char> msg = PatternMessage(8000, 10);
	SendFromA(msg);

	ASSERT_TRUE(PumpUntilReceived(1, 150000));
	EXPECT_EQ(msg, received[0]);
	ASSERT_LT(a.GetCurrentMtuBytes(), TEST_MTU) << "no step-down happened";

	for (size_t i = 0; i < aToBLog.size(); i++)
	{
		EXPECT_LE((int) aToBLog[i].bytes.size(), aToBLog[i].mtuAtSend - UDP_HEADER_SIZE) << "datagram " << i;
		EXPECT_LE((int) aToBLog[i].bytes.size(), aToBLog[i].mtuAtSend);
	}
	EXPECT_LE(a.GetCurrentMtuBytes(), 1100 + UDP_HEADER_SIZE);
}

// The largest datagram a 20000-byte reliable ordered message produces on the
// wire without encryption, measured on the build before datagram protection
// existed: split fragments fill the MTU budget minus 8 bytes of header slack.
const int kPlaintextLargestDatagram = TEST_MTU - UDP_HEADER_SIZE - 8;

class ReliabilityLayerPlaintext : public ReliabilityLayerCrypto
{
protected:
	virtual void SetUp()
	{
		ReliabilityLayerPair::SetUp();
		ResetLayers(EncryptionMode::Plaintext);
	}
};

TEST_F(ReliabilityLayerPlaintext, PlaintextModeKeepsTodaysWireFormat)
{
	ASSERT_FALSE(a.IsEncrypted());

	std::vector<unsigned char> msg = PatternMessage(20000, 11);
	SendFromA(msg);
	ASSERT_TRUE(PumpUntilReceived(1, 30000));
	EXPECT_EQ(msg, received[0]);

	// No prefix, no tag: the payload travels in clear...
	bool sawPayload = false;
	for (size_t i = 0; i < aToBLog.size() && !sawPayload; i++)
		sawPayload = ContainsAnyWindowOf(aToBLog[i].bytes, msg);
	EXPECT_TRUE(sawPayload);
	// ...and no datagram budget is reduced by the cipher overhead: split
	// fragments fill the full pre-encryption budget.
	EXPECT_EQ(kPlaintextLargestDatagram, (int) LargestLoggedDatagram());
	EXPECT_EQ(0u, DroppedUnauthenticatedAtB());
}

// Mode Required without keys fails closed: the layer neither emits nor accepts
// a cleartext datagram, so a connection installed without its session keys can
// never run unencrypted.
TEST_F(ReliabilityLayerPlaintext, RequiredModeWithoutKeysNeverEmitsCleartext)
{
	ResetLayers(EncryptionMode::Required);
	ASSERT_FALSE(a.IsEncrypted());

	SendFromA(PatternMessage(20000, 12));
	SendFromA(PatternMessage(100, 13), Reliability::Unreliable);
	for (int i = 0; i < 200; i++)
		Tick();

	EXPECT_TRUE(aToBLog.empty()) << aToBLog.size() << " datagrams left in clear";
	EXPECT_GT(a.GetDroppedKeylessSendsForTesting(), 0u);
	EXPECT_TRUE(received.empty());
}

TEST_F(ReliabilityLayerPlaintext, RequiredModeWithoutKeysNeverAcceptsCleartext)
{
	// Valid cleartext datagrams, captured from a Plaintext sender...
	SendFromA(PatternMessage(3000, 14));
	a.Update(&aSock, bAddr, TEST_MTU, now + 10000, 0, handlers, &rnr, updateBitStream);
	std::vector<std::vector<char>> cleartext = aSock.sent;
	aSock.sent.clear();
	ASSERT_FALSE(cleartext.empty());

	// ...are all dropped and counted by a receiver in mode Required without keys.
	b.SetEncryptionMode(EncryptionMode::Required);
	b.Reset(true, TEST_MTU);
	b.SetTimeoutTime(TEST_TIMEOUT_MS);
	ReanchorClock(); // else b.Update below may return early and prove nothing
	for (size_t i = 0; i < cleartext.size(); i++)
		DeliverToB(cleartext[i]);
	unsigned char *data = nullptr;
	EXPECT_EQ(0u, b.Receive(&data));
	EXPECT_EQ((uint64_t) cleartext.size(), DroppedUnauthenticatedAtB());
	// Not even an acknowledgement goes back.
	b.Update(&bSock, aAddr, TEST_MTU, now + 20000, 0, handlers, &rnr, updateBitStream);
	EXPECT_TRUE(bSock.sent.empty());
}

TEST(ReliabilityLayerCryptoKeys, SetSessionKeysFailsOutsideModeRequired)
{
	ReliabilityLayer layer;
	layer.SetEncryptionMode(EncryptionMode::Plaintext);
	layer.Reset(true, MAXIMUM_MTU_SIZE);
	SessionKeys keys;
	memset(keys.send, 1, sizeof(keys.send));
	memset(keys.recv, 2, sizeof(keys.recv));
	EXPECT_FALSE(layer.SetSessionKeys(keys));
	EXPECT_FALSE(layer.IsEncrypted());

	// The mode decides at Reset() whether the overhead is reserved: switching to
	// Required without a Reset() is not enough either.
	layer.SetEncryptionMode(EncryptionMode::Required);
	EXPECT_FALSE(layer.SetSessionKeys(keys));
	layer.Reset(true, MAXIMUM_MTU_SIZE);
	EXPECT_TRUE(layer.SetSessionKeys(keys));
	EXPECT_TRUE(layer.IsEncrypted());
}

TEST(ReliabilityLayerCryptoStats, SumsDroppedUnauthenticatedDatagrams)
{
	RakNetStatistics total;
	RakNetStatistics one;
	memset(&total, 0, sizeof(total));
	memset(&one, 0, sizeof(one));
	total.droppedUnauthenticatedDatagrams = 3;
	one.droppedUnauthenticatedDatagrams = 4;
	total += one;
	EXPECT_EQ(7u, total.droppedUnauthenticatedDatagrams);
}

TEST(ReliabilityLayerCryptoStats, StatisticsToStringPrintsDroppedUnauthenticatedDatagrams)
{
	RakNetStatistics stats;
	memset(&stats, 0, sizeof(stats));
	stats.droppedUnauthenticatedDatagrams = 4242;
	for (int verbosity = 1; verbosity <= 2; verbosity++)
	{
		char buffer[8192];
		StatisticsToString(&stats, buffer, sizeof(buffer), verbosity);
		EXPECT_NE(nullptr, strstr(buffer, "4242")) << "verbosity " << verbosity;
	}
}

TEST(ReliabilityLayerCryptoKeys, ResetWipesTheSessionKeys)
{
	ReliabilityLayer layer;
	layer.SetEncryptionMode(EncryptionMode::Required);
	layer.Reset(true, MAXIMUM_MTU_SIZE);
	SessionKeys keys;
	memset(keys.send, 1, sizeof(keys.send));
	memset(keys.recv, 2, sizeof(keys.recv));
	ASSERT_TRUE(layer.SetSessionKeys(keys));
	EXPECT_TRUE(layer.IsEncrypted());
	layer.Reset(false, MAXIMUM_MTU_SIZE);
	EXPECT_FALSE(layer.IsEncrypted());
}

} // namespace
