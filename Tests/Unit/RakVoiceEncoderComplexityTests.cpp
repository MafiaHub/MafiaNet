/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// RakVoice::SetEncoderComplexity: the setting reaches encoders already open and those
// opened later, read back from Opus itself.

#include <gtest/gtest.h>

#include "mafianet/voice.h"

#include <opus.h>

using namespace MafiaNet;

namespace
{
class ComplexityProbe : public RakVoice
{
public:
	using RakVoice::GetOrCreateChannel;
};

int ComplexityOf(VoiceChannel *channel)
{
	opus_int32 complexity = -1;
	EXPECT_EQ(opus_encoder_ctl(channel->encoder, OPUS_GET_COMPLEXITY(&complexity)), OPUS_OK);
	return complexity;
}

// What a fresh Opus encoder uses when nobody sets a complexity.
int OpusDefaultComplexity()
{
	int error = OPUS_OK;
	OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
	EXPECT_EQ(error, OPUS_OK);
	opus_int32 complexity = -1;
	opus_encoder_ctl(encoder, OPUS_GET_COMPLEXITY(&complexity));
	opus_encoder_destroy(encoder);
	return complexity;
}
} // namespace

TEST(RakVoiceEncoderComplexity, LeavesTheOpusDefaultUntilSet)
{
	ComplexityProbe voice;
	voice.Init(48000, 960 * sizeof(short));
	VoiceChannel *channel = voice.GetOrCreateChannel(RakNetGUID(0x7000ull));
	ASSERT_NE(channel, nullptr);
	ASSERT_NE(channel->encoder, nullptr);
	EXPECT_EQ(ComplexityOf(channel), OpusDefaultComplexity());
	voice.Deinit();
}

TEST(RakVoiceEncoderComplexity, AppliesToOpenAndLaterEncoders)
{
	ComplexityProbe voice;
	voice.Init(48000, 960 * sizeof(short));
	VoiceChannel *open = voice.GetOrCreateChannel(RakNetGUID(0x7100ull));
	ASSERT_NE(open, nullptr);
	voice.SetEncoderComplexity(3);
	EXPECT_EQ(ComplexityOf(open), 3);
	VoiceChannel *later = voice.GetOrCreateChannel(RakNetGUID(0x7101ull));
	ASSERT_NE(later, nullptr);
	EXPECT_EQ(ComplexityOf(later), 3);
	voice.SetEncoderComplexity(0);
	EXPECT_EQ(ComplexityOf(open), 0);
	EXPECT_EQ(ComplexityOf(later), 0);
	voice.Deinit();
}

TEST(RakVoiceEncoderComplexity, SetBeforeInitAppliesToChannelsOpenedAfter)
{
	ComplexityProbe voice;
	voice.SetEncoderComplexity(5);
	voice.Init(48000, 960 * sizeof(short));
	VoiceChannel *channel = voice.GetOrCreateChannel(RakNetGUID(0x7200ull));
	ASSERT_NE(channel, nullptr);
	EXPECT_EQ(ComplexityOf(channel), 5);
	voice.Deinit();
}

TEST(RakVoiceEncoderComplexity, ClampsValuesAboveTen)
{
	ComplexityProbe voice;
	voice.Init(48000, 960 * sizeof(short));
	VoiceChannel *open = voice.GetOrCreateChannel(RakNetGUID(0x7300ull));
	ASSERT_NE(open, nullptr);
	voice.SetEncoderComplexity(2);
	voice.SetEncoderComplexity(42);
	EXPECT_EQ(ComplexityOf(open), 10);
	VoiceChannel *later = voice.GetOrCreateChannel(RakNetGUID(0x7301ull));
	ASSERT_NE(later, nullptr);
	EXPECT_EQ(ComplexityOf(later), 10);
	voice.Deinit();
}

// Negative mirrors SetEncoderBitrate(0): future encoders get the Opus default, open ones keep
// what was last applied.
TEST(RakVoiceEncoderComplexity, NegativeRestoresTheOpusDefaultForLaterEncodersOnly)
{
	ComplexityProbe voice;
	voice.Init(48000, 960 * sizeof(short));
	voice.SetEncoderComplexity(1);
	VoiceChannel *open = voice.GetOrCreateChannel(RakNetGUID(0x7400ull));
	ASSERT_NE(open, nullptr);
	voice.SetEncoderComplexity(-1);
	EXPECT_EQ(ComplexityOf(open), 1);
	VoiceChannel *later = voice.GetOrCreateChannel(RakNetGUID(0x7401ull));
	ASSERT_NE(later, nullptr);
	EXPECT_EQ(ComplexityOf(later), OpusDefaultComplexity());
	voice.Deinit();
}
