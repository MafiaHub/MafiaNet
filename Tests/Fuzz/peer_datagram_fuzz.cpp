/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Fuzzes a whole RakPeer server with datagrams from a remote that need not
 *  follow the protocol: offline messages (ping, open-connection requests),
 *  the connection handshake, and -- once the input replays a handshake, as
 *  the seed corpus does -- everything a connected peer can send.
 *
 *  Runs on the SimulatedNetwork (no sockets, no threads, virtual clock), so
 *  each input is a deterministic, self-contained run. Input is a sequence of
 *  datagrams, each prefixed by a 16-bit little-endian length, all sent from
 *  10.0.0.2:60000 to the server at 10.0.0.1:60000 (the addresses the seeds
 *  were recorded with). The server runs in plaintext mode with an all-zero
 *  challenge secret (see below); the seeds are protocol 10 handshakes recorded
 *  under that secret.
 */

#include <cstddef>
#include <cstdint>

#include "SimulatedNetwork.h"

#include "mafianet/handshake_challenge.h"
#include "mafianet/mtu_size.h"

using namespace MafiaNet;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	SimulatedNetwork net(1);
	// Plaintext, so the seeds' datagrams are not sealed (the fuzzer cannot forge
	// an AEAD tag): the harness reaches the handshake and the connected parsers.
	// session_crypto_fuzz covers the cipher itself.
	net.SetNewPeerEncryptionMode(EncryptionMode::Plaintext);
	RakPeer *server = net.AddPeer(4, 4);
	// The open-connection challenge is keyed per process by default. A fixed
	// (all-zero) secret makes it a pure function of the simulated clock and the
	// remote's address, so a recorded REQUEST_2 stays valid when replayed.
	const uint8_t zeroSecret[16] = {};
	server->GetHandshakeChallengeForTesting().SetSecretForTesting(zeroSecret);
	const SystemAddress remote("10.0.0.2", 60000);

	size_t offset = 0;
	while (offset + 2 <= size)
	{
		size_t length = (size_t) data[offset] | ((size_t) data[offset + 1] << 8);
		offset += 2;
		if (length > size - offset)
			length = size - offset;
		if (length > MAXIMUM_MTU_SIZE)
			length = MAXIMUM_MTU_SIZE;
		if (length > 0)
			net.InjectDatagram(remote, server, data + offset, (int) length);
		offset += length;
		net.Step(10);
	}
	// Let timers run: resends, timeouts, connection cleanup.
	net.Step(200);
	return 0;
}
