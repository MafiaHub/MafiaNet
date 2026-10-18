/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>

#include <cstdint>

#include "mafianet/bit_stream.h"
#include "mafianet/string.h"
#include "mafianet/string_compressor.h"

using namespace MafiaNet;

namespace
{
	// A typical entity-state message: id, position, compressed orientation,
	// flags and a few range-packed values.
	void WriteEntityState(BitStream &bs, uint32_t i)
	{
		bs.Write((uint8_t) 135);
		bs.Write(i);
		bs.Write(1.5f * i);
		bs.Write(2.5f * i);
		bs.Write(-3.5f * i);
		bs.WriteNormQuat(1.f, 0.f, 0.f, 0.f);
		bs.Write((i & 1) != 0);
		bs.Write((i & 2) != 0);
		bs.WriteCompressed((uint16_t) (i & 0xfff));
		bs.WriteBitsFromIntegerRange((uint32_t) (i % 100), 0u, 100u);
	}

	void ReadEntityState(BitStream &bs, benchmark::State &state)
	{
		uint8_t id;
		uint32_t index;
		float x, y, z, qw, qx, qy, qz;
		bool a, b;
		uint16_t c;
		uint32_t r;
		if (!(bs.Read(id) && bs.Read(index) && bs.Read(x) && bs.Read(y) && bs.Read(z) &&
			bs.ReadNormQuat(qw, qx, qy, qz) && bs.Read(a) && bs.Read(b) && bs.ReadCompressed(c) &&
			bs.ReadBitsFromIntegerRange(r, 0u, 100u)))
		{
			state.SkipWithError("read failed");
			return;
		}
		benchmark::DoNotOptimize(x + y + z + qw + c + r);
	}
}

static void BitStream_WriteEntityState(benchmark::State &state)
{
	BitStream bs(1024);
	uint32_t i = 0;
	for (auto _ : state)
	{
		bs.Reset();
		WriteEntityState(bs, i++);
		benchmark::DoNotOptimize(bs.GetData());
	}
	state.SetBytesProcessed(state.iterations() * bs.GetNumberOfBytesUsed());
}
BENCHMARK(BitStream_WriteEntityState);

static void BitStream_ReadEntityState(benchmark::State &state)
{
	BitStream written;
	WriteEntityState(written, 42);
	for (auto _ : state)
	{
		BitStream bs(written.GetData(), written.GetNumberOfBytesUsed(), false);
		ReadEntityState(bs, state);
	}
	state.SetBytesProcessed(state.iterations() * written.GetNumberOfBytesUsed());
}
BENCHMARK(BitStream_ReadEntityState);

static void BitStream_RakStringCompressedRoundTrip(benchmark::State &state)
{
	StringCompressor::AddReference();
	const RakString text("player joined the server and said hello to everyone in the lobby");
	BitStream bs(256);
	for (auto _ : state)
	{
		bs.Reset();
		text.SerializeCompressed(&bs);
		RakString out;
		out.DeserializeCompressed(&bs);
		benchmark::DoNotOptimize(out.C_String());
	}
	StringCompressor::RemoveReference();
}
BENCHMARK(BitStream_RakStringCompressedRoundTrip);

static void BitStream_WriteAlignedBytes(benchmark::State &state)
{
	const size_t size = (size_t) state.range(0);
	std::vector<unsigned char> payload(size, 0xAB);
	BitStream bs((unsigned int) size + 16);
	for (auto _ : state)
	{
		bs.Reset();
		bs.Write(true); // misalign, as real messages usually are
		bs.WriteAlignedBytes(payload.data(), (unsigned int) size);
		benchmark::DoNotOptimize(bs.GetData());
	}
	state.SetBytesProcessed(state.iterations() * (int64_t) size);
}
BENCHMARK(BitStream_WriteAlignedBytes)->Arg(64)->Arg(1400)->Arg(64 * 1024);
