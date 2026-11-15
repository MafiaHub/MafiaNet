/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace MafiaNet
{
// The body of the server_reply fuzz harness (Tests/Fuzz/server_reply_fuzz.cpp), shared with the unit test that
// keeps its seeds' depth from rotting.
//
// A plaintext RakPeer client calls Connect() to 10.0.0.2:60000 on the SimulatedNetwork and is fed the input as
// datagrams from that address. Input: one control byte (bit 0: pin the identity public key of the Ed25519 seed
// 00..1f), then datagrams, each prefixed by a 16-bit little-endian length.
//
// For the duration of the run libsodium's random source is a fixed stream, reset per call, so the client's
// handshake nonce -- which REPLY_2 echoes and the identity signature covers -- is the same on every run and a
// recorded REPLY_2 still verifies. The OS random source (randombytes_sysrandom_implementation) is installed
// again before returning.
struct ServerReplyOutcome
{
	bool started = false;           // the input had a control byte and Connect() started
	bool accepted = false;          // ID_CONNECTION_REQUEST_ACCEPTED
	bool mismatch = false;          // ID_PUBLIC_KEY_MISMATCH
	bool remoteIdentityKey = false; // GetRemoteIdentityKey() for the server
};

ServerReplyOutcome RunServerReplyInput(const uint8_t *data, size_t size);
} // namespace MafiaNet
