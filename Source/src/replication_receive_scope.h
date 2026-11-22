// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/peer_interface.h"
#include <optional>

namespace MafiaNet
{
// A state callback holds the sender and replica locks. Perform a shutdown it
// requests only after those locks have been released, before Receive returns.
class ReplicationReceiveScope
{
public:
	explicit ReplicationReceiveScope(RakPeerInterface &target)
	    : peer(target), previous(current) { current = this; }
	~ReplicationReceiveScope()
	{
		current = previous;
		if (shutdown)
			peer.Shutdown(shutdown->duration, shutdown->channel, shutdown->priority);
	}
	ReplicationReceiveScope(const ReplicationReceiveScope &) = delete;
	ReplicationReceiveScope &operator=(const ReplicationReceiveScope &) = delete;

	static bool DeferShutdown(RakPeerInterface &target, unsigned duration, unsigned char channel, Priority priority)
	{
		if (auto *scope = Find(target))
		{
			scope->shutdown = ShutdownRequest{duration, channel, priority};
			return true;
		}
		return false;
	}
	static bool IsClosing(const RakPeerInterface &target)
	{
		const auto *scope = Find(target);
		return scope && scope->shutdown.has_value();
	}
	bool IsClosing() const { return shutdown.has_value(); }

private:
	struct ShutdownRequest
	{
		unsigned duration;
		unsigned char channel;
		Priority priority;
	};
	static ReplicationReceiveScope *Find(const RakPeerInterface &target)
	{
		for (auto *scope = current; scope; scope = scope->previous)
			if (&scope->peer == &target)
				return scope;
		return nullptr;
	}
	inline static thread_local ReplicationReceiveScope *current = nullptr;
	RakPeerInterface &peer;
	ReplicationReceiveScope *previous;
	std::optional<ShutdownRequest> shutdown;
};
}
