/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file SessionAdmission.h
/// \brief The slot accounting RakPeer applies to remote-initiated peers, as pure functions.
///
/// Kept out of RakPeer so the rules are unit tested directly rather than only through loopback traffic.
/// RakPeer gathers the counts on the network thread and asks these; they hold no state of their own.

#pragma once

namespace MafiaNet
{
namespace SessionAdmission
{
	/// What a server has configured. maxPending==0 means no pending pool (SetMaximumPendingSessions not used).
	struct Limits
	{
		unsigned int maxIncoming = 0;
		unsigned int maxPending = 0;
		unsigned int maxPendingPerAddress = 0; ///< 0: no per-address bound
	};

	/// Remote-initiated peers, as the network thread sees them when a new one asks to connect.
	struct Counts
	{
		unsigned int connected = 0;          ///< CONNECTED
		unsigned int exchanging = 0;         ///< EXCHANGING_SESSION_DATA
		unsigned int pending = 0;            ///< every state before CONNECTED, EXCHANGING_SESSION_DATA included
		unsigned int pendingFromAddress = 0; ///< those of \a pending sharing the newcomer's IP
	};

	/// True when the server keeps peers mid-handshake in a pool of their own.
	inline bool UsesPendingPool(const Limits &limits)
	{
		return limits.maxPending != 0;
	}

	/// May a new peer start connecting?
	///
	/// Without a pool this is the historic rule: a peer mid-handshake already owns a slot, so it counts
	/// against the incoming limit alongside connected peers. With a pool, the incoming limit is the
	/// players' and is enforced at acceptance instead (MayAccept), so a full server can still hold and
	/// queue newcomers; only the pool and the per-address share bound who may start.
	inline bool MayStartConnecting(const Limits &limits, const Counts &counts)
	{
		if (!UsesPendingPool(limits))
			return counts.connected + counts.exchanging < limits.maxIncoming;
		if (counts.pending >= limits.maxPending)
			return false;
		return limits.maxPendingPerAddress == 0 || counts.pendingFromAddress < limits.maxPendingPerAddress;
	}

	/// May an application's AcceptSession() promote one more peer to CONNECTED?
	///
	/// Without a pool the peer's slot was taken when it started connecting, so acceptance is always
	/// within the limit. With a pool it is checked here, because the handshake was allowed to start
	/// while the server was full.
	inline bool MayAccept(const Limits &limits, const Counts &counts)
	{
		return !UsesPendingPool(limits) || counts.connected < limits.maxIncoming;
	}
} // namespace SessionAdmission
} // namespace MafiaNet
