/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/native_feature_includes.h"

#if _RAKNET_SUPPORT_PacketLogger == 1

#include <string>
#include <vector>

#include "mafianet/threadsafe_packet_logger.h"

using namespace MafiaNet;

namespace
{
// Records what reaches WriteLog instead of printing it.
class RecordingLogger : public ThreadsafePacketLogger
{
public:
	void Log(const char *str) { AddToLog(str); }
	void WriteLog(const char *str) override { written.push_back(str); }

	std::vector<std::string> written;
};
}

TEST(ThreadsafePacketLogger, LinesAreWrittenOnlyByUpdateInOrder)
{
	RecordingLogger logger;
	logger.Log("first");
	logger.Log("second");
	EXPECT_TRUE(logger.written.empty());

	logger.Update();
	ASSERT_EQ(logger.written.size(), 2u);
	EXPECT_EQ(logger.written[0], "first");
	EXPECT_EQ(logger.written[1], "second");

	// Nothing is written twice.
	logger.Update();
	EXPECT_EQ(logger.written.size(), 2u);
}

TEST(ThreadsafePacketLogger, ManyLinesBetweenUpdatesAreAllKept)
{
	// More than the old ring buffer's initial capacity, to cover growth.
	RecordingLogger logger;
	for (int i = 0; i < 100; i++)
		logger.Log(std::to_string(i).c_str());
	logger.Update();
	ASSERT_EQ(logger.written.size(), 100u);
	for (int i = 0; i < 100; i++)
		EXPECT_EQ(logger.written[i], std::to_string(i));
}

TEST(ThreadsafePacketLogger, DestroyingWithUnwrittenLinesFreesThem)
{
	// Run under ASan/LSan: the queued strings must not leak.
	RecordingLogger logger;
	logger.Log("never written");
}

#endif
