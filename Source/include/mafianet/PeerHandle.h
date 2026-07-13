/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file PeerHandle.h
/// \brief RAII handles for the two core MafiaNet resources.
///
/// \ref MafiaNet::Peer owns a RakPeerInterface instance (GetInstance/DestroyInstance);
/// \ref MafiaNet::PacketPtr owns a Packet received from it (Receive/DeallocatePacket).
/// Both are movable, non-copyable and exception-safe. This is purely additive —
/// the raw factory/Receive API remains available and unchanged.

#pragma once

#include <utility> // std::move

#include "mafianet/peerinterface.h" // RakPeerInterface, Receive, DeallocatePacket, GetInstance, DestroyInstance
#include "mafianet/types.h"         // Packet
#include "mafianet/Export.h"        // RAK_DLL_EXPORT

namespace MafiaNet {

class IncomingRange; // Range adaptor returned by Peer::incoming(); defined below.

/// \brief Owning handle for a Packet returned by RakPeerInterface::Receive().
/// Calls DeallocatePacket() in its destructor. Movable, non-copyable.
class RAK_DLL_EXPORT PacketPtr {
public:
    /// Takes ownership of \a p, which must have been produced by \a owner.
    PacketPtr(RakPeerInterface* owner, Packet* p) : owner_(owner), p_(p) {}
    ~PacketPtr();

    PacketPtr(PacketPtr&& o) noexcept;
    PacketPtr& operator=(PacketPtr&& o) noexcept;
    PacketPtr(const PacketPtr&) = delete;
    PacketPtr& operator=(const PacketPtr&) = delete;

    Packet* operator->() const { return p_; }
    Packet& operator*()  const { return *p_; }
    Packet* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    /// Message identifier, accounting for an ID_TIMESTAMP prefix.
    /// Returns 255 when the packet is null or empty.
    unsigned char id() const;

private:
    RakPeerInterface* owner_;
    Packet* p_;
};

/// \brief Owning handle for a RakPeerInterface instance.
/// Calls DestroyInstance() in its destructor. Movable, non-copyable.
class RAK_DLL_EXPORT Peer {
public:
    Peer() : raw_(RakPeerInterface::GetInstance()) {}
    ~Peer();

    Peer(Peer&& o) noexcept;
    Peer& operator=(Peer&& o) noexcept;
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    RakPeerInterface* operator->() const { return raw_; }
    RakPeerInterface* get() const { return raw_; }
    /// False only for a moved-from (and thus empty) handle.
    explicit operator bool() const { return raw_ != nullptr; }

    /// Receive the next queued packet wrapped in a PacketPtr (may be empty).
    PacketPtr receive();

    /// \brief Range adaptor that drains the receive queue in a range-based for.
    ///
    /// Each iteration yields a fresh PacketPtr that deallocates when the loop
    /// body scope ends — the range equivalent of the classic
    /// `for (p = Receive(); p; DeallocatePacket(p), p = Receive())` loop:
    /// \code
    ///     for (auto pkt : peer.incoming()) {
    ///         switch (pkt.id()) { ... }   // pkt frees at end of each iteration
    ///     }
    /// \endcode
    /// Single-pass input range: a UDP receive queue cannot be rewound.
    IncomingRange incoming();

private:
    RakPeerInterface* raw_;
};

/// \brief Single-pass input range over a Peer's receive queue (see Peer::incoming()).
///
/// Header-only and lightweight: it holds only a Peer pointer. begin() performs
/// one receive(), operator++ performs the next, and end() is a null sentinel —
/// so iteration stops as soon as the queue is drained. Dereferencing transfers
/// ownership of the current packet to the caller, matching the per-iteration
/// DeallocatePacket of the legacy loop.
class IncomingRange {
public:
    explicit IncomingRange(Peer* peer) : peer_(peer) {}

    /// Null sentinel compared against the iterator once the queue is empty.
    struct EndSentinel {};

    class Iterator {
    public:
        Iterator(Peer* peer, PacketPtr pkt) : peer_(peer), cur_(std::move(pkt)) {}

        // Move-only, mirroring PacketPtr — an input iterator is never copied.
        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;
        Iterator(Iterator&&) = default;
        Iterator& operator=(Iterator&&) = default;

        /// Hands the current packet to the loop body, which owns (and frees) it.
        PacketPtr operator*() { return std::move(cur_); }

        /// Pull the next packet from the queue.
        Iterator& operator++() { cur_ = peer_->receive(); return *this; }

        bool operator!=(EndSentinel) const { return static_cast<bool>(cur_); }
        bool operator==(EndSentinel) const { return !static_cast<bool>(cur_); }

    private:
        Peer* peer_;
        PacketPtr cur_;
    };

    Iterator begin() { return Iterator(peer_, peer_->receive()); }
    EndSentinel end() const { return EndSentinel{}; }

private:
    Peer* peer_;
};

inline IncomingRange Peer::incoming() { return IncomingRange(this); }

} // namespace MafiaNet
