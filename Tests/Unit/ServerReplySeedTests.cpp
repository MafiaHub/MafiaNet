/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The server_reply fuzz seeds replay a recorded handshake, and only stay
 *  valid while the client draws its random numbers in the same order as when
 *  they were recorded. FuzzCorpus.server_reply only checks that they do not
 *  crash; these tests check they still reach as deep as they were recorded to.
 *  If one fails after a change to what the client draws from libsodium's
 *  random source, re-record the seeds (see Tests/Support/ServerReplyHarness.h).
 */

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ServerReplyHarness.h"

using namespace MafiaNet;

namespace
{
std::vector<uint8_t> ReadSeed(const char *name)
{
	const std::string path = std::string(MAFIANET_FUZZ_CORPUS_DIR) + "/server_reply/" + name;
	std::ifstream in(path, std::ios::binary);
	EXPECT_TRUE(in.good()) << "cannot read " << path;
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST(ServerReplySeeds, PinnedOkReachesTheConnection)
{
	const std::vector<uint8_t> seed = ReadSeed("pinned_ok");
	ASSERT_FALSE(seed.empty());
	const ServerReplyOutcome outcome = RunServerReplyInput(seed.data(), seed.size());
	EXPECT_TRUE(outcome.started);
	EXPECT_TRUE(outcome.accepted) << "pinned_ok no longer reaches ID_CONNECTION_REQUEST_ACCEPTED: re-record the seeds";
	EXPECT_TRUE(outcome.remoteIdentityKey);
	EXPECT_FALSE(outcome.mismatch);
}

TEST(ServerReplySeeds, PinnedBadSignatureReachesTheMismatch)
{
	const std::vector<uint8_t> seed = ReadSeed("pinned_bad_signature");
	ASSERT_FALSE(seed.empty());
	const ServerReplyOutcome outcome = RunServerReplyInput(seed.data(), seed.size());
	EXPECT_TRUE(outcome.started);
	EXPECT_TRUE(outcome.mismatch) << "pinned_bad_signature no longer reaches ID_PUBLIC_KEY_MISMATCH: re-record the seeds";
	EXPECT_FALSE(outcome.accepted);
}
