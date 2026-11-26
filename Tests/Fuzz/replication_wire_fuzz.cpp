// Copyright (c) 2026, MafiaHub. MIT license.
#include "../../Source/src/replication_wire.h"
#include <cstddef>
#include <cstdint>
using namespace MafiaNet;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *bytes, size_t size)
{
	if (!size || size > ReplicationWire::MaxPayloadBits / 8 + 256)
		return 0;
	BitStream in(const_cast<unsigned char *>(bytes), static_cast<unsigned>(size), false);
	ReplicationWire::Header header{};
	if (!ReplicationWire::ReadHeader(in, header) || header.version != ReplicationWire::Version)
		return 0;
	if (header.kind == ReplicationWire::Kind::StateBatch || header.kind == ReplicationWire::Kind::SnapshotBatch)
	{
		ReplicationWire::StateBatchView batch;
		batch.Read(in, header.kind == ReplicationWire::Kind::SnapshotBatch);
		return 0;
	}
	if (ReplicationWire::IsSnapshotAck(header.kind))
	{
		ReplicationWire::SnapshotAckWindow acks;
		acks.Read(in, header.kind);
		return 0;
	}
	if (header.kind == ReplicationWire::Kind::ClockSample)
	{
		ReplicationWire::ClockSample sample{};
		sample.Read(in);
		return 0;
	}
	if (header.kind == ReplicationWire::Kind::Decline)
	{
		ReplicationWire::Decline decline;
		decline.Read(in);
		return 0;
	}
	NetworkID id;
	uint64_t visit;
	uint32_t type;
	BitStream snapshot;
	if (header.kind == ReplicationWire::Kind::Spawn && in.Read(id) && in.Read(visit) && in.Read(type))
		ReplicationWire::ReadBlock(in, snapshot);
	return 0;
}
