/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "ServerReplyHarness.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sodium.h>

// sodium.h declares the implementation structs. The internal one is declared in
// randombytes_internal_random.h (with "salsa20" as an alias), as in Ubuntu 24.04's libsodium 1.0.18;
// older releases shipped only randombytes_salsa20_random.h. Detect which header exists.
#if defined(__has_include)
#if __has_include(<sodium/randombytes_internal_random.h>)
#define MAFIANET_HAVE_SODIUM_INTERNAL_RANDOM 1
#elif __has_include(<sodium/randombytes_salsa20_random.h>)
#define MAFIANET_HAVE_SODIUM_SALSA20_RANDOM 1
#endif
#endif

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

// libsodium documents that randombytes_set_implementation must be called before sodium_init, so this
// switchable implementation is installed once, by a static initializer, before any sodium_init in the
// process. It is never swapped again: a RunServerReplyInput scope only flips g_fixedActive. While
// inactive every call is forwarded to the implementation libsodium would have used by default, so a
// binary that links this file draws OS randomness as usual outside a run.
//
// Which implementation that is depends on the libsodium version and build: upstream's
// randombytes.c defaults to randombytes_sysrandom_implementation (read in 1.0.18 and 1.0.22, and
// what Ubuntu 24.04 reports), but a build can choose another default with
// RANDOMBYTES_DEFAULT_IMPLEMENTATION, and older releases name the internal one "salsa20". So the default is not hard-coded: it is asked for with
// randombytes_implementation_name() and mapped to the exported struct of that name.
//
// Asking first is safe. randombytes_implementation_name() calls randombytes_init_if_needed(), which
// points the (static) implementation pointer at the default and stirs it, but
// randombytes_set_implementation assigns that pointer unconditionally, with no "already initialised"
// check, so a later install simply replaces the default (read from randombytes.c in libsodium 1.0.18
// and 1.0.22; sodium_init only calls randombytes_stir). The name query does not lock the default in.
//
// An unknown name leaves g_realRandom null: nothing is installed and RunServerReplyInput aborts with a
// message instead of silently running on, or replacing, the process RNG.
const randombytes_implementation *DefaultRandomImplementation()
{
	const char *name = randombytes_implementation_name();
	if (name == nullptr)
		return nullptr;
	if (std::strcmp(name, "sysrandom") == 0)
		return &randombytes_sysrandom_implementation;
#if defined(MAFIANET_HAVE_SODIUM_INTERNAL_RANDOM)
	if (std::strcmp(name, "internal") == 0)
		return &randombytes_internal_implementation;
#endif
#if defined(MAFIANET_HAVE_SODIUM_SALSA20_RANDOM)
	if (std::strcmp(name, "salsa20") == 0)
		return &randombytes_salsa20_implementation;
#endif
	std::fprintf(stderr, "ServerReplyHarness: unknown libsodium default RNG \"%s\"; fixed-stream mode is unavailable\n", name);
	return nullptr;
}
const randombytes_implementation *const g_realRandom = DefaultRandomImplementation();
std::atomic<bool> g_fixedActive{false};

const char *FixedName()
{
	return g_fixedActive.load(std::memory_order_relaxed) ? "mafianet-fuzz-fixed" : g_realRandom->implementation_name();
}
uint32_t FixedRandom()
{
	return g_fixedActive.load(std::memory_order_relaxed) ? (uint32_t) NextRandom() : g_realRandom->random();
}
void FixedStir()
{
	// Always forwarded, so the real implementation is seeded by sodium_init and ready when inactive.
	g_realRandom->stir();
}
void FixedBuf(void *const buf, const size_t size)
{
	if (!g_fixedActive.load(std::memory_order_relaxed))
	{
		g_realRandom->buf(buf, size);
		return;
	}
	uint8_t *out = static_cast<uint8_t *>(buf);
	for (size_t i = 0; i < size; i += 8)
	{
		const uint64_t r = NextRandom();
		std::memcpy(out + i, &r, size - i < 8 ? size - i : 8);
	}
}
int FixedClose()
{
	return g_realRandom->close();
}

// uniform is left null: libsodium's default builds it on random() above.
randombytes_implementation g_fixedRandom = {FixedName, FixedRandom, FixedStir, nullptr, FixedBuf, FixedClose};

// Runs before main and therefore before any sodium_init in this process.
const bool g_fixedInstalled = g_realRandom != nullptr && randombytes_set_implementation(&g_fixedRandom) == 0;

// Makes the installed implementation produce the fixed stream for one run.
struct FixedRandomScope
{
	FixedRandomScope()
	{
		g_fixedActive.store(true, std::memory_order_relaxed);
		g_rngState = 0;
	}
	~FixedRandomScope() { g_fixedActive.store(false, std::memory_order_relaxed); }
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
	if (!g_fixedInstalled)
	{
		std::fprintf(stderr, "ServerReplyHarness: the fixed random stream is not installed; refusing to run on the process RNG\n");
		std::abort();
	}
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
