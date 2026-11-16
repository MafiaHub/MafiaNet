/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Shared pieces of the soak tests: the numbered message format and the
 *  in-order checker, a process-wide live-allocation counter, and the
 *  environment knobs.
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

namespace Soak
{
// A reliable-ordered message: id | session (u64) | sequence (u32) | extra length (u16) | extra bytes.
// The extra bytes are a function of (session, sequence, index), so a split message is checked byte by byte.
const MafiaNet::MessageID ID_SOAK_ORDERED = ID_USER_PACKET_ENUM;
// An unreliable message: id | anything. Only counted.
const MafiaNet::MessageID ID_SOAK_UNRELIABLE = ID_USER_PACKET_ENUM + 1;
// A reliable (unordered) message, in the ordered message's format: each must arrive exactly once.
const MafiaNet::MessageID ID_SOAK_RELIABLE = ID_USER_PACKET_ENUM + 2;

inline unsigned char ExtraByte(uint64_t session, uint32_t sequence, uint32_t index)
{
	return (unsigned char) ((session >> (8 * (index % 8))) ^ (sequence * 31u) ^ (index * 7u));
}

void WriteOrdered(MafiaNet::BitStream &out, uint64_t session, uint32_t sequence, uint16_t extraBytes);
void WriteReliable(MafiaNet::BitStream &out, uint64_t session, uint32_t sequence, uint16_t extraBytes);

// One direction of one connection, on the sending side. A new session starts each time the sender sees the
// connection established; its sequence numbers start at 0.
struct SendStream
{
	bool live = false;
	uint64_t session = 0;
	uint32_t next = 0;
};

// The same direction on the receiving side.
struct RecvStream
{
	uint64_t session = 0;
	uint32_t expected = 0;
};

// Checks every ordered message a receiver gets: within a session the sequence numbers are 0, 1, 2, ... with
// no gap and no repeat, a new session starts at 0, and the extra bytes are intact. Violations are counted
// (with the first few described) rather than asserted one by one, so a broken run does not flood the log.
class OrderChecker
{
public:
	// \a data / \a length: the whole message, starting with ID_SOAK_ORDERED.
	void OnOrdered(int receiver, int sender, const unsigned char *data, size_t length);
	const RecvStream *Stream(int receiver, int sender) const;
	uint64_t Violations() const { return violations; }
	uint64_t Delivered() const { return delivered; }
	const std::vector<std::string> &Examples() const { return examples; }

private:
	void Violation(const std::string &what);
	std::map<std::pair<int, int>, RecvStream> streams;
	uint64_t violations = 0;
	uint64_t delivered = 0;
	std::vector<std::string> examples;
};

// Checks every reliable (unordered) message a receiver gets: within a session each sequence number arrives
// exactly once, in any order, with its extra bytes intact. A new session replaces the old one.
class ExactlyOnceChecker
{
public:
	struct Stream
	{
		uint64_t session = 0;
		uint32_t received = 0;     // distinct sequence numbers
		std::vector<bool> arrived; // by sequence number
	};

	// \a data / \a length: the whole message, starting with ID_SOAK_RELIABLE.
	void OnReliable(int receiver, int sender, const unsigned char *data, size_t length);
	const Stream *Find(int receiver, int sender) const;
	uint64_t Violations() const { return violations; }
	uint64_t Delivered() const { return delivered; }
	const std::vector<std::string> &Examples() const { return examples; }

private:
	void Violation(const std::string &what);
	std::map<std::pair<int, int>, Stream> streams;
	uint64_t violations = 0;
	uint64_t delivered = 0;
	std::vector<std::string> examples;
};

// Live allocations made through operator new and through MafiaNet's rakMalloc_Ex / rakRealloc_Ex /
// rakFree_Ex, whose hooks SoakSupport.cpp installs before main(). Counts blocks (and, for operator new,
// bytes where the platform reports a block's size). Process wide and thread safe.
struct AllocationSnapshot
{
	int64_t newBlocks = 0;
	int64_t newBytes = 0;
	int64_t rakBlocks = 0;
};
// False in a sanitizer build, where operator new is the sanitizer's: newBlocks / newBytes then stay 0.
bool CountsOperatorNew();
AllocationSnapshot Allocations();

// Environment knobs, read once per call.
uint32_t EnvUInt(const char *name, uint32_t fallback);
} // namespace Soak
