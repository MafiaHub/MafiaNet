// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/voice.h"
#include <benchmark/benchmark.h>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace MafiaNet;

namespace
{
class EncodeProbe : public RakVoice
{
public:
	using RakVoice::EncodeFrame;
	using RakVoice::GetOrCreateChannel;
};

// Encodes one 20 ms, 48 kHz mono frame of a voiced signal per iteration. The arg is the
// SetEncoderComplexity value; -1 never calls it, leaving the Opus default.
void VoiceEncodeFrame(benchmark::State &state)
{
	EncodeProbe voice;
	voice.Init(48000, 960 * sizeof(short));
	voice.SetVAD(false);
	voice.SetEncoderBitrate(24000);
	if (state.range(0) >= 0)
		voice.SetEncoderComplexity(static_cast<int>(state.range(0)));
	VoiceChannel *channel = voice.GetOrCreateChannel(RakNetGUID(0x8000ull));
	if (channel == nullptr)
	{
		state.SkipWithError("channel setup failed");
		return;
	}
	// One second of a syllable-shaped harmonic signal, cycled.
	constexpr unsigned kFrames = 50;
	std::vector<short> signal(kFrames * 960);
	for (unsigned i = 0; i < signal.size(); i++)
	{
		const double t = i / 48000.0;
		const double envelope = 0.6 + 0.4 * std::sin(2.0 * 3.14159265358979 * 4.0 * t);
		double sample = 0.0;
		for (int harmonic = 1; harmonic <= 12; harmonic++)
			sample += std::sin(2.0 * 3.14159265358979 * 140.0 * harmonic * t) / harmonic;
		signal[i] = static_cast<short>(6000.0 * envelope * sample);
	}
	std::vector<short> frame(960);
	unsigned char packet[1500];
	unsigned index = 0;
	int64_t bytes = 0;
	for (auto _ : state)
	{
		// EncodeFrame may modify the samples (noise filter), so each frame starts from a copy.
		std::copy(signal.begin() + index * 960, signal.begin() + (index + 1) * 960, frame.begin());
		index = (index + 1) % kFrames;
		bytes += voice.EncodeFrame(channel, frame.data(), packet, sizeof(packet));
		benchmark::DoNotOptimize(packet);
	}
	state.counters["bytes/frame"] = double(bytes) / state.iterations();
	voice.Deinit();
}
BENCHMARK(VoiceEncodeFrame)->ArgName("complexity")->Arg(-1)->Arg(10)->Arg(0)->Unit(benchmark::kMicrosecond);
}
