/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Per-connection session crypto: ChaCha20-Poly1305 datagram sealing/opening
 *  and the once-per-connection key agreement.
 */

#include <benchmark/benchmark.h>

#include <vector>

#include "mafianet/session_crypto.h"

using namespace MafiaNet;

namespace
{
const RakNetGUID kInitiatorGuid(0x1111222233334444ull);
const RakNetGUID kResponderGuid(0x5555666677778888ull);
} // namespace

// Seal one datagram on the sender, then open it on the receiver. Each iteration
// seals a fresh sequence number, so the receiver's replay window always accepts it.
static void BM_SealOpen(benchmark::State &state)
{
	EphemeralKeyPair i, r;
	const CryptInfo infoI = i.MakeCryptInfo();
	const CryptInfo infoR = r.MakeCryptInfo();
	SessionKeys initiatorKeys, responderKeys;
	if (!DeriveSessionKeys(i, true, infoI, infoR, kInitiatorGuid, kResponderGuid, &initiatorKeys) ||
	    !DeriveSessionKeys(r, false, infoI, infoR, kInitiatorGuid, kResponderGuid, &responderKeys))
	{
		state.SkipWithError("key agreement failed");
		return;
	}
	DatagramCipher sender, receiver;
	sender.SetKeys(initiatorKeys);
	receiver.SetKeys(responderKeys);

	const int len = (int) state.range(0);
	std::vector<uint8_t> plaintext((size_t) len, 0x5a);
	std::vector<uint8_t> sealed((size_t) len + DatagramCipher::kMaxOverhead);
	for (auto _ : state)
	{
		const int n = sender.Seal(plaintext.data(), len, sealed.data(), (int) sealed.size());
		uint8_t *opened = nullptr;
		const int m = receiver.Open(sealed.data(), n, &opened);
		if (n <= 0 || m != len)
		{
			state.SkipWithError("seal/open round trip failed");
			break;
		}
		benchmark::DoNotOptimize(opened);
	}
	state.SetBytesProcessed(state.iterations() * len);
}
BENCHMARK(BM_SealOpen)->Arg(64)->Arg(512)->Arg(1200);

// One side's whole handshake cost: generate an ephemeral key pair, then derive the
// session keys from the peer's public key. DeriveSessionKeys consumes (wipes) the
// local pair, so key pair generation is part of every iteration. The remote side's
// pair is built outside the timed region.
static void BM_DeriveSessionKeys(benchmark::State &state)
{
	for (auto _ : state)
	{
		state.PauseTiming();
		EphemeralKeyPair remote;
		const CryptInfo infoR = remote.MakeCryptInfo();
		state.ResumeTiming();

		EphemeralKeyPair local;
		const CryptInfo infoI = local.MakeCryptInfo();
		SessionKeys keys;
		if (!DeriveSessionKeys(local, true, infoI, infoR, kInitiatorGuid, kResponderGuid, &keys))
		{
			state.SkipWithError("DeriveSessionKeys failed");
			break;
		}
		benchmark::DoNotOptimize(keys);
	}
}
BENCHMARK(BM_DeriveSessionKeys);
