// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include <array>

namespace MafiaNet::ReplicationWire
{
constexpr uint8_t Version = 2;
constexpr BitSize_t MaxPayloadBits = 8 * 1024 * 1024;
enum class Kind : uint8_t
{
	Hello,
	Accept,
	Reject,
	Spawn,
	State,
	Leave,
	Destroy,
	Bind,
	Grant,
	Revoke,
	OwnerUpdate,
	Snapshot,
	SnapshotAck,
	StateBatch,
	SnapshotBatch
};

constexpr unsigned HeaderBytes = 3 + 2 * sizeof(uint64_t);
constexpr unsigned MaxBatchBytes = 1200;
constexpr unsigned MaxBatchRecords = 32;

struct StateRecord
{
	unsigned char *data;
	uint16_t bits;
};

// Bodies start at byte boundaries. Validate all record boundaries before
// handing any of the borrowed views to the receiving game.
struct StateBatchView
{
	std::array<StateRecord, MaxBatchRecords> records;
	unsigned count = 0;
	bool Read(BitStream &in)
	{
		count = 0;
		if (in.GetNumberOfBitsUsed() > MaxBatchBytes * 8 || in.GetReadOffset() % 8)
			return false;
		while (in.GetNumberOfUnreadBits())
		{
			uint16_t bits;
			if (count == records.size() || !in.Read(bits) || bits < (sizeof(NetworkID) + 2 * sizeof(uint64_t)) * 8)
				return false;
			const auto padded = BITS_TO_BYTES(bits) * 8u;
			if (padded > in.GetNumberOfUnreadBits())
				return false;
			records[count++] = {in.GetData() + in.GetReadOffset() / 8, bits};
			in.IgnoreBits(padded);
		}
		return count >= 2;
	}
};

struct Header
{
	uint8_t version;
	Kind kind;
	uint64_t clientToken, serverToken;
};
inline bool ReadHeader(BitStream &in, Header &header)
{
	MessageID id;
	return in.Read(id) && id == ID_REPLICATION && in.Read(header.version) && in.Read(header.kind) &&
	       in.Read(header.clientToken) && in.Read(header.serverToken);
}
inline void WriteHeader(BitStream &out, Kind kind, uint64_t clientToken, uint64_t serverToken)
{
	out.Write(static_cast<MessageID>(ID_REPLICATION));
	out.Write(Version);
	out.Write(kind);
	out.Write(clientToken);
	out.Write(serverToken);
}

inline bool WriteBlock(BitStream &out, const BitStream &data)
{
	const auto bits = data.GetNumberOfBitsUsed();
	if (bits > MaxPayloadBits)
		return false;
	out.Write(bits);
	if (bits)
		out.WriteBits(data.GetData(), bits, false);
	return true;
}
inline bool ReadBlock(BitStream &in, BitStream &data)
{
	BitSize_t bits;
	return in.Read(bits) && bits <= MaxPayloadBits && bits <= in.GetNumberOfUnreadBits() && (!bits || in.Read(data, bits));
}
}
