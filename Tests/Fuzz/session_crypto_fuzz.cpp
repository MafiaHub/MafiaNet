/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Fuzzes DatagramCipher, the AEAD layer every connected datagram passes
 *  through. Two properties:
 *
 *   - Open() on arbitrary bytes never crashes and never reads out of bounds
 *     (a forged or malformed datagram is rejected, not trusted).
 *   - Whatever the input, sealing it on one cipher and opening it on the
 *     paired one gives the input back.
 *
 *  Keys are fixed bytes (no randomness), so every run is deterministic.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mafianet/session_crypto.h"

using namespace MafiaNet;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	// An initiator/responder pair: what one seals the other opens.
	SessionKeys initiator;
	SessionKeys responder;
	for (int i = 0; i < 32; ++i)
	{
		initiator.send[i] = (uint8_t) (0x10 + i);
		initiator.recv[i] = (uint8_t) (0x80 + i);
	}
	std::memcpy(responder.send, initiator.recv, 32);
	std::memcpy(responder.recv, initiator.send, 32);

	DatagramCipher sender;
	DatagramCipher receiver;
	DatagramCipher roundTripReceiver;
	sender.SetKeys(initiator);
	receiver.SetKeys(responder);
	roundTripReceiver.SetKeys(responder);

	// 1. Arbitrary bytes as a received datagram. Open() works in place.
	std::vector<uint8_t> datagram(data, data + size);
	uint8_t *plaintext = nullptr;
	const int opened = receiver.Open(datagram.data(), (int) datagram.size(), &plaintext);
	if (opened >= 0 && (plaintext < datagram.data() || plaintext + opened > datagram.data() + datagram.size()))
		std::abort(); // an accepted datagram's plaintext must lie inside the buffer

	// 2. Seal the first 512 bytes and open them on the paired cipher.
	const int plainLen = (int) (size < 512 ? size : 512);
	std::vector<uint8_t> sealed((size_t) plainLen + DatagramCipher::kMaxOverhead);
	const int sealedLen = sender.Seal(data, plainLen, sealed.data(), (int) sealed.size());
	if (sealedLen < 0)
		std::abort();
	uint8_t *roundTripped = nullptr;
	const int roundTripLen = roundTripReceiver.Open(sealed.data(), sealedLen, &roundTripped);
	if (roundTripLen != plainLen || (plainLen > 0 && std::memcmp(roundTripped, data, (size_t) plainLen) != 0))
		std::abort();
	return 0;
}
