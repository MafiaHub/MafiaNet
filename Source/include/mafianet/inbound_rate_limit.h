// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once

#include "mafianet/time.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

namespace MafiaNet
{
/// What a peer does with a connection that sends more than InboundRateLimit::flood_per_second messages.
enum class FloodAction : uint8_t
{
	/// Drop the connection's application messages until its one-second window ends.
	Drop,
	/// Drop them and close the connection, sending InboundRateLimit::flood_reason in the disconnection
	/// notification. The local application receives ID_DISCONNECTION_NOTIFICATION with the same reason and
	/// Packet::disconnectCause set to DisconnectCause::InboundFlood.
	Disconnect
};

/// Per-connection limit on the application messages a peer accepts from each remote.
///
/// Application messages are the ones at ID_USER_PACKET_ENUM and above, also behind an ID_TIMESTAMP prefix. The
/// network thread counts them after the reliability layer reassembles and orders them, before they are queued for
/// Receive(), so a dropped message never reaches the application, its plugins or the queue. RakPeer's own
/// messages and those of built-in services and plugins are never counted or dropped.
///
/// Every limit is off at zero; a default-constructed limit disables the feature.
struct InboundRateLimit
{
	/// Sustained rate. Messages beyond it are dropped and counted in RakNetStatistics::rateLimitedMessages.
	uint32_t messages_per_second = 0;
	/// Messages a connection may send at once above the sustained rate; zero means messages_per_second.
	/// A connection starts with a full burst.
	uint32_t burst = 0;
	/// More messages than this within one second, dropped ones included, is a flood: counted in
	/// RakNetStatistics::inboundFloods and handled as flood_action says.
	uint32_t flood_per_second = 0;
	FloodAction flood_action = FloodAction::Drop;
	/// Sent with the disconnection notification when flood_action is Disconnect. Opaque to MafiaNet.
	std::string flood_reason;

	bool enabled() const { return messages_per_second != 0 || flood_per_second != 0; }
	/// A burst needs a rate to refill it.
	bool valid() const { return (burst == 0 || messages_per_second != 0) && (flood_action == FloodAction::Drop || flood_action == FloodAction::Disconnect); }
};

/// The accounting behind InboundRateLimit for one connection: a token bucket for the sustained rate and a fixed
/// one-second window for the flood threshold. Pure: the caller supplies the time. Driven by one thread; the
/// counters may be read from any.
class InboundRateLimiter
{
public:
	enum class Verdict
	{
		Accept,
		Drop,
		/// This message crossed the flood threshold. It is dropped too. Returned once per window.
		Flood
	};

	void Reset()
	{
		started = false;
		tokens = 0;
		lastRefill = 0;
		windowStart = 0;
		windowCount = 0;
		flooding = false;
		dropped.store(0, std::memory_order_relaxed);
		floods.store(0, std::memory_order_relaxed);
	}

	Verdict OnMessage(const InboundRateLimit &limit, TimeUS now)
	{
		if (!started || now - windowStart >= kWindowUs || now < windowStart)
		{
			windowStart = now;
			windowCount = 0;
			flooding = false;
		}
		if (windowCount != UINT32_MAX)
			++windowCount;
		if (flooding)
			return Drop();
		if (limit.flood_per_second != 0 && windowCount > limit.flood_per_second)
		{
			flooding = true;
			floods.fetch_add(1, std::memory_order_relaxed);
			Drop();
			return Verdict::Flood;
		}

		if (limit.messages_per_second == 0)
		{
			started = true;
			return Verdict::Accept;
		}
		// Tokens are kept in millionths of a message, so a rate in messages per second refills `rate` of them per
		// microsecond, with no rounding. The bucket holds burst messages.
		const uint64_t capacity = static_cast<uint64_t>(limit.burst != 0 ? limit.burst : limit.messages_per_second) * kUnit;
		if (!started)
			tokens = capacity;
		else if (now > lastRefill)
		{
			// Bound the elapsed time first so the product cannot overflow: anything past a full refill is moot.
			const uint64_t elapsed = std::min<uint64_t>(now - lastRefill, capacity / limit.messages_per_second + 1);
			tokens = std::min(capacity, tokens + elapsed * limit.messages_per_second);
		}
		tokens = std::min(tokens, capacity);
		lastRefill = now;
		started = true;
		if (tokens < kUnit)
			return Drop();
		tokens -= kUnit;
		return Verdict::Accept;
	}

	uint64_t Dropped() const { return dropped.load(std::memory_order_relaxed); }
	uint64_t Floods() const { return floods.load(std::memory_order_relaxed); }

private:
	static constexpr uint64_t kUnit = 1000000;
	static constexpr TimeUS kWindowUs = 1000000;

	Verdict Drop()
	{
		dropped.fetch_add(1, std::memory_order_relaxed);
		return Verdict::Drop;
	}

	bool started = false;
	uint64_t tokens = 0;
	TimeUS lastRefill = 0;
	TimeUS windowStart = 0;
	uint32_t windowCount = 0;
	bool flooding = false;
	std::atomic<uint64_t> dropped{0};
	std::atomic<uint64_t> floods{0};
};
}
