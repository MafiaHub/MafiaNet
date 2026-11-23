// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include <array>
#include <limits>
#include <type_traits>

namespace MafiaNet::ReplicationWire
{
constexpr uint8_t Version = 7;
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
	SnapshotBatch,
	OwnerCorrection,
	ClockSample
};

constexpr unsigned HeaderBytes = 3 + 2 * sizeof(uint64_t);
constexpr unsigned MaxBatchBytes = 1200;
constexpr unsigned MaxBatchRecords = 32;

// Canonical unsigned LEB128. Bound both the number of bytes and the high bits
// of the final byte for the destination type.
inline unsigned VarBytes(uint64_t value)
{
	unsigned bytes = 1;
	while (value >= 128)
	{
		value >>= 7;
		++bytes;
	}
	return bytes;
}
inline void WriteVar(BitStream &out, uint64_t value)
{
	while (value >= 128)
	{
		out.Write(static_cast<uint8_t>((value & 127) | 128));
		value >>= 7;
	}
	out.Write(static_cast<uint8_t>(value));
}
template <class T>
bool ReadVar(BitStream &in, T &value)
{
	static_assert(std::is_unsigned_v<T>);
	T result = 0;
	for (unsigned shift = 0; shift < std::numeric_limits<T>::digits; shift += 7)
	{
		uint8_t byte;
		if (!in.Read(byte))
			return false;
		const T low = byte & 127u;
		if (low > (std::numeric_limits<T>::max() >> shift))
			return false;
		result = static_cast<T>(result | (low << shift));
		if (!(byte & 128u))
		{
			if (shift && !low)
				return false;
			value = result;
			return true;
		}
	}
	return false;
}

// Bit i confirms first + i only. The oldest confirmed sequence anchors each
// window, so a rejected or stale record creates a hole without any history table.
struct SnapshotAckWindow
{
	uint64_t first = 0;
	uint32_t mask = 0;
	bool Read(BitStream &in)
	{
		if (!ReadVar(in, first) || !first || !ReadVar(in, mask) || !(mask & 1u) || in.GetNumberOfUnreadBits())
			return false;
		const auto remaining = UINT64_MAX - first;
		return remaining >= 31 || !(mask >> (remaining + 1));
	}
};
struct SnapshotAcks
{
	std::array<uint64_t, MaxBatchRecords> sequences;
	unsigned count = 0;
};

struct ClockSample
{
	Time tick;
	TimeUS sent;
	bool Read(BitStream &in)
	{
		return in.Read(tick) && tick && in.Read(sent) && !in.GetNumberOfUnreadBits();
	}
};

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
	uint64_t firstSequence = 0;
	Time timestamp = 0;
	bool Read(BitStream &in, bool snapshots = false)
	{
		count = 0;
		if (in.GetNumberOfBitsUsed() > MaxBatchBytes * 8 || in.GetReadOffset() % 8)
			return false;
		if (snapshots && (!ReadVar(in, firstSequence) || !firstSequence || !ReadVar(in, timestamp)))
			return false;
		while (in.GetNumberOfUnreadBits())
		{
			uint16_t bits;
			if (count == records.size() || !(snapshots ? ReadVar(in, bits) : in.Read(bits)) || bits < (snapshots ? 9u : (sizeof(NetworkID) + 2 * sizeof(uint64_t)) * 8))
				return false;
			const auto padded = BITS_TO_BYTES(bits) * 8u;
			if (padded > in.GetNumberOfUnreadBits())
				return false;
			records[count++] = {in.GetData() + in.GetReadOffset() / 8, bits};
			in.IgnoreBits(padded);
		}
		return count >= 2 && (!snapshots || UINT64_MAX - firstSequence >= count - 1);
	}
};

// The high bit distinguishes snapshot traffic on an established encrypted
// session from the version byte of a full replication header.
constexpr uint8_t SnapshotTag = 0x80;
static_assert(Version < SnapshotTag);
inline bool IsSnapshotMessage(Kind kind)
{
	return kind == Kind::Snapshot || kind == Kind::SnapshotBatch || kind == Kind::SnapshotAck;
}
inline unsigned WireHeaderBytes(Kind kind, uint64_t context)
{
	return context && IsSnapshotMessage(kind) ? 2 + VarBytes(context) : HeaderBytes;
}
struct Header
{
	uint8_t version;
	Kind kind;
	uint64_t clientToken = 0, serverToken = 0, snapshotContext = 0;
};
inline bool ReadHeader(BitStream &in, Header &header)
{
	MessageID id;
	uint8_t tag;
	if (!in.Read(id) || id != ID_REPLICATION || !in.Read(tag))
		return false;
	if (tag & SnapshotTag)
	{
		header = {Version, static_cast<Kind>(tag & ~SnapshotTag)};
		return IsSnapshotMessage(header.kind) && ReadVar(in, header.snapshotContext) && header.snapshotContext != 0;
	}
	header.version = tag;
	header.snapshotContext = 0;
	return in.Read(header.kind) && in.Read(header.clientToken) && in.Read(header.serverToken);
}
inline void WriteHeader(BitStream &out, Kind kind, uint64_t clientToken, uint64_t serverToken, uint64_t context = 0)
{
	out.Write(static_cast<MessageID>(ID_REPLICATION));
	if (context && IsSnapshotMessage(kind))
	{
		out.Write(static_cast<uint8_t>(SnapshotTag | static_cast<uint8_t>(kind)));
		WriteVar(out, context);
		return;
	}
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
