/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Fuzzes the connecting side of the handshake: a RakPeer client that has
 *  called Connect() is fed datagrams from the address it connects to, so the
 *  input plays the server -- ID_OPEN_CONNECTION_REPLY_1, REPLY_2 with its
 *  server crypt info and identity block (the signature check, the pinned-key
 *  compare), the refusal IDs, and, once a seed replays a whole handshake, what
 *  a connected server sends.
 *
 *  The body lives in Tests/Support/ServerReplyHarness.cpp, which documents the
 *  input format and the fixed random stream that keeps recorded seeds valid.
 *  ServerReplySeedTests (a unit test) replays two seeds through it and checks
 *  they still reach the connection and the identity mismatch.
 */

#include <cstddef>
#include <cstdint>

#include "ServerReplyHarness.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	(void) MafiaNet::RunServerReplyInput(data, size);
	return 0;
}
