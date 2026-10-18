/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Fuzzes BitStream deserialization: every Read* that game code applies to
 *  bytes received from the network. The first byte of each step picks the
 *  operation, so the fuzzer explores sequences of reads over one stream, as a
 *  real message handler does. Only bounded/safe read APIs are driven; the
 *  legacy unbounded ones (Read(char*), Read(wchar_t*&)) are unsafe by contract.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "mafianet/bit_stream.h"
#include "mafianet/string_compressor.h"
#include "mafianet/string.h"
#include "mafianet/wstring.h"

using namespace MafiaNet;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	// DeserializeCompressed uses the shared Huffman tables, which normally
	// exist because a RakPeer holds a reference. Hold one for the process.
	static const bool compressorReady = (StringCompressor::AddReference(), true);
	(void) compressorReady;

	BitStream in(const_cast<unsigned char *>(data), (unsigned int) size, false);

	for (int step = 0; step < 64 && in.GetNumberOfUnreadBits() > 0; ++step)
	{
		uint8_t op = 0;
		if (!in.Read(op))
			break;

		switch (op % 20)
		{
		case 0:
		{
			uint8_t v;
			in.Read(v);
			break;
		}
		case 1:
		{
			uint32_t v;
			in.Read(v);
			break;
		}
		case 2:
		{
			uint64_t v;
			in.ReadCompressed(v);
			break;
		}
		case 3:
		{
			int32_t v;
			in.ReadCompressed(v);
			break;
		}
		case 4:
		{
			float v;
			in.ReadCompressed(v);
			break;
		}
		case 5:
		{
			double v;
			in.Read(v);
			break;
		}
		case 6:
		{
			uint16_t v = 0;
			in.ReadDelta(v);
			break;
		}
		case 7:
		{
			uint32_t v = 0;
			in.ReadCompressedDelta(v);
			break;
		}
		case 8:
		{
			RakString s;
			s.Deserialize(&in);
			break;
		}
		case 9:
		{
			RakString s;
			s.DeserializeCompressed(&in, op & 0x80);
			break;
		}
		case 10:
		{
			RakWString s;
			s.Deserialize(&in);
			break;
		}
		case 11:
		{
			char *buf = nullptr;
			unsigned int len = 0;
			in.ReadAlignedBytesSafeAlloc(&buf, len, 4096);
			rakFree_Ex(buf, _FILE_AND_LINE_);
			break;
		}
		case 12:
		{
			char buf[64];
			unsigned int len = 0;
			in.ReadAlignedBytesSafe(buf, len, sizeof(buf));
			break;
		}
		case 13:
		{
			uint32_t v;
			in.ReadBitsFromIntegerRange(v, 10u, 5000u);
			break;
		}
		case 14:
		{
			float x, y, z;
			in.ReadNormVector(x, y, z);
			break;
		}
		case 15:
		{
			float x, y, z;
			in.ReadVector(x, y, z);
			break;
		}
		case 16:
		{
			float w, x, y, z;
			in.ReadNormQuat(w, x, y, z);
			break;
		}
		case 17:
		{
			float m[9];
			in.ReadOrthMatrix(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
			break;
		}
		case 18:
		{
			// Nested stream: what message handlers do with sub-payloads.
			BitStream sub;
			uint16_t bits = 0;
			if (in.Read(bits))
				in.Read(sub, bits % 4096);
			break;
		}
		case 19:
		{
			unsigned char bytes[16];
			in.ReadBits(bytes, op % 128, (op & 1) != 0);
			break;
		}
		}
	}
	return 0;
}
