// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

namespace MafiaNet::ReplicationWire
{
constexpr uint8_t Version = 1;
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
	SnapshotAck
};

inline void WriteHeader(BitStream &out, Kind kind, uint64_t clientToken, uint64_t serverToken, uint8_t world = 0)
{
	out.Write(static_cast<MessageID>(ID_REPLICATION));
	out.Write(Version);
	out.Write(kind);
	out.Write(clientToken);
	out.Write(serverToken);
	out.Write(world);
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
