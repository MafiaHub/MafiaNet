/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "ServerReplyHarness.h"

#include <cstring>

#include <sodium.h>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_size.h"
#include "mafianet/session_crypto.h"

namespace MafiaNet
{
namespace
{
// splitmix64, reset per input.
uint64_t g_rngState = 0;

uint64_t NextRandom()
{
	uint64_t z = (g_rngState += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

const char *FixedName()
{
	return "mafianet-fuzz-fixed";
}
uint32_t FixedRandom()
{
	return (uint32_t) NextRandom();
}
void FixedStir()
{
}
void FixedBuf(void *const buf, const size_t size)
{
	uint8_t *out = static_cast<uint8_t *>(buf);
	for (size_t i = 0; i < size; i += 8)
	{
		const uint64_t r = NextRandom();
		std::memcpy(out + i, &r, size - i < 8 ? size - i : 8);
	}
}
int FixedClose()
{
	return 0;
}

randombytes_implementation g_fixedRandom = {FixedName, FixedRandom, FixedStir, nullptr, FixedBuf, FixedClose};

// Installs the fixed stream for one run and the OS source again afterwards.
struct FixedRandomScope
{
	FixedRandomScope()
	{
		randombytes_set_implementation(&g_fixedRandom);
		g_rngState = 0;
	}
	~FixedRandomScope() { randombytes_set_implementation(&randombytes_sysrandom_implementation); }
	FixedRandomScope(const FixedRandomScope &) = delete;
	FixedRandomScope &operator=(const FixedRandomScope &) = delete;
};

// The identity public key of the seed 00..1f (crypto_sign_ed25519_seed_keypair).
const uint8_t kPinnedIdentity[32] = {0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe, 0x1d, 0x70, 0xdd, 0x18, 0xe7, 0x4b, 0xc0, 0x99,
                                     0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5, 0x0d, 0x5f, 0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55, 0x31, 0xb8};
} // namespace

ServerReplyOutcome RunServerReplyInput(const uint8_t *data, size_t size)
{
	ServerReplyOutcome outcome;
	if (size < 1 || sodium_init() < 0)
		return outcome;
	FixedRandomScope fixedRandom;

	SimulatedNetwork net(1);
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	RakPeer *client = net.AddPeer(1);
	const SystemAddress server("10.0.0.2", 60000);
	PublicKey pin;
	std::memcpy(pin.serverIdentity, kPinnedIdentity, sizeof(pin.serverIdentity));
	if (client->Connect("10.0.0.2", 60000, nullptr, 0, (data[0] & 1) ? &pin : nullptr) != CONNECTION_ATTEMPT_STARTED)
		return outcome;
	outcome.started = true;
	net.Step(10);

	size_t offset = 1;
	while (offset + 2 <= size)
	{
		size_t length = (size_t) data[offset] | ((size_t) data[offset + 1] << 8);
		offset += 2;
		if (length > size - offset)
			length = size - offset;
		if (length > MAXIMUM_MTU_SIZE)
			length = MAXIMUM_MTU_SIZE;
		if (length > 0)
			net.InjectDatagram(server, client, data + offset, (int) length);
		offset += length;
		net.Step(10);
	}
	// Let timers run: resends, timeouts, connection cleanup.
	net.Step(200);

	outcome.accepted = net.Received(client, ID_CONNECTION_REQUEST_ACCEPTED);
	outcome.mismatch = net.Received(client, ID_PUBLIC_KEY_MISMATCH);
	uint8_t key[32];
	outcome.remoteIdentityKey = client->GetRemoteIdentityKey(server, key);
	return outcome;
}
} // namespace MafiaNet
