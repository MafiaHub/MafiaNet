// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include <cstdint>

namespace MafiaNet
{
/// Reserved callers may use any connected slot. Public callers share connected - reserved slots.
enum class ConnectionSlot
{
	Public,
	Reserved
};

/// Accepted claims a slot immediately. Full leaves the request pending for another decision.
enum class SessionAcceptResult
{
	Accepted,
	Full,
	Expired,
	InvalidPayload,
	OutOfMemory
};

struct ServerCapacity
{
	unsigned int connected = 1;
	unsigned int pending = 32;
	unsigned int reserved = 0;
	unsigned int pending_per_address = 0; ///< Zero allows the entire pending pool to share one IP.

	bool valid() const
	{
		return connected > 0 && connected <= UINT16_MAX && pending > 0 && pending <= UINT16_MAX - connected && reserved <= connected && pending_per_address <= pending;
	}
	bool allows_pending(unsigned int waiting, unsigned int from_address) const
	{
		return connected > 0 && waiting < pending && (pending_per_address == 0 || from_address < pending_per_address);
	}
};
}
