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

#include <optional> // std::optional (Result storage for a move-only Peer)
#include <string>   // std::string (owned password / bind-address in the builders)
#include <utility>  // std::move

#include "mafianet/peerinterface.h" // RakPeerInterface, Receive, DeallocatePacket, GetInstance, DestroyInstance
#include "mafianet/types.h"         // Packet, StartupResult, ConnectionAttemptResult, SocketDescriptor, PublicKey
#include "mafianet/Export.h"        // RAK_DLL_EXPORT

namespace MafiaNet {

class IncomingRange; // Range adaptor returned by Peer::incoming(); defined below.
class ServerBuilder; // Fluent Startup builder for a listening Peer; defined below.
class ClientBuilder; // Fluent Startup(+Connect) builder for a connecting Peer; defined below.

/// \brief Which stage of the builder pipeline reported a failure (see \ref PeerError).
enum class PeerStage {
    Startup, ///< RakPeerInterface::Startup() (or InitializeSecurity) failed.
    Connect  ///< RakPeerInterface::Connect() failed (client builder only).
};

/// \brief The failure carried by a non-ok \ref Result from a builder's start().
///
/// The underlying engine enum is preserved rather than collapsed to a bool:
/// inspect \ref stage to know which member is meaningful. On a \ref
/// PeerStage::Startup failure \ref startup holds the offending StartupResult
/// (and \ref connect stays CONNECTION_ATTEMPT_STARTED); on a \ref
/// PeerStage::Connect failure \ref connect holds the ConnectionAttemptResult.
struct PeerError {
    PeerStage stage = PeerStage::Startup;
    StartupResult startup = RAKNET_STARTED;
    ConnectionAttemptResult connect = CONNECTION_ATTEMPT_STARTED;
};

/// \brief Success-or-error holder returned by ServerBuilder / ClientBuilder start().
///
/// Header-only and move-only (it may own a move-only \ref Peer). Test success
/// with `if (result)` / is_ok(), reach the value with `*result` / value(), and
/// read the underlying engine enum via error() on failure.
/// \code
///     auto r = Peer::server().port(60000).max_connections(32).start();
///     if (!r) { handle(r.error().startup); return; }
///     Peer server = std::move(*r);
/// \endcode
template <class T>
class Result {
public:
    /// Build a success result owning \a value.
    static Result ok(T value) {
        Result r;
        r.value_.emplace(std::move(value));
        return r;
    }
    /// Build a failure result carrying \a error (no value present).
    static Result err(PeerError error) {
        Result r;
        r.error_ = error;
        return r;
    }

    /// True when a value is present.
    bool is_ok() const { return value_.has_value(); }
    /// Same as is_ok(); enables `if (result)`.
    explicit operator bool() const { return value_.has_value(); }

    /// Access the value. Precondition: is_ok().
    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }
    T* operator->() { return &*value_; }
    const T* operator->() const { return &*value_; }

    /// The failure detail. Meaningful only when !is_ok().
    const PeerError& error() const { return error_; }

private:
    std::optional<T> value_;
    PeerError error_;
};

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

    /// \brief Begin building a listening (server) Peer.
    /// \details Chain .port()/.max_connections()/… then .start(), which performs
    /// Startup + SetMaximumIncomingConnections in one call. See \ref ServerBuilder.
    static ServerBuilder server();

    /// \brief Begin building a connecting (client) Peer.
    /// \details Chain .max_connections()/.connect()/… then .start(), which performs
    /// Startup (and Connect if a target was given). See \ref ClientBuilder.
    static ClientBuilder client();

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

/// \brief Fluent builder for a listening Peer (see Peer::server()).
///
/// Replaces the manual Startup + SetMaximumIncomingConnections (+ optional
/// SetIncomingPassword / InitializeSecurity) dance with a single chain:
/// \code
///     auto r = Peer::server()
///         .port(60000)
///         .max_connections(32)
///         .incoming_password("hunter2")   // optional
///         .start();                        // Result<Peer>
/// \endcode
/// The builder owns its SocketDescriptor until start(), so the caller never
/// manages the array/count. Header-only: it only forwards to the exported API.
class ServerBuilder {
public:
    /// Local port to listen on (0 lets the OS pick — unusual for a server).
    ServerBuilder& port(unsigned short p) { port_ = p; return *this; }

    /// Bind to a specific local address (e.g. "127.0.0.1"). Default: INADDR_ANY.
    ServerBuilder& host(const char* addr) { host_ = addr ? addr : ""; return *this; }

    /// Maximum simultaneous connections; also the incoming-connection cap.
    ServerBuilder& max_connections(unsigned int n) { maxConnections_ = n; return *this; }

    /// Optional low-level password incoming connections must match.
    ServerBuilder& incoming_password(const char* pw) { password_ = pw ? pw : ""; return *this; }

    /// \brief Opt-in encryption for incoming connections (InitializeSecurity).
    /// \details Mirrors master: security is off unless you call this. Has effect
    /// only when LIBCAT_SECURITY==1; the pointers must outlive start(). Does not
    /// make encryption mandatory for anything else.
    ServerBuilder& secure(const char* publicKey, const char* privateKey, bool requireClientKey = false) {
        publicKey_ = publicKey;
        privateKey_ = privateKey;
        requireClientKey_ = requireClientKey;
        secure_ = true;
        return *this;
    }

    /// \brief Start up and begin accepting connections.
    /// \return Result carrying the live Peer on success, or the failing
    /// StartupResult (PeerStage::Startup) otherwise.
    Result<Peer> start() {
        Peer peer;
        SocketDescriptor sd(port_, host_.empty() ? nullptr : host_.c_str());
#if LIBCAT_SECURITY == 1
        if (secure_) {
            if (!peer->InitializeSecurity(publicKey_, privateKey_, requireClientKey_)) {
                PeerError e;
                e.stage = PeerStage::Startup;
                e.startup = STARTUP_OTHER_FAILURE;
                return Result<Peer>::err(e);
            }
        }
#endif
        StartupResult sr = peer->Startup(maxConnections_, &sd, 1);
        if (sr != RAKNET_STARTED) {
            PeerError e;
            e.stage = PeerStage::Startup;
            e.startup = sr;
            return Result<Peer>::err(e);
        }
        if (!password_.empty())
            peer->SetIncomingPassword(password_.c_str(), (int) password_.size());
        peer->SetMaximumIncomingConnections((unsigned short) maxConnections_);
        return Result<Peer>::ok(std::move(peer));
    }

private:
    unsigned short port_ = 0;
    unsigned int maxConnections_ = 1;
    std::string host_;
    std::string password_;
    bool secure_ = false;
    const char* publicKey_ = nullptr;
    const char* privateKey_ = nullptr;
    bool requireClientKey_ = false;
};

/// \brief Fluent builder for a connecting Peer (see Peer::client()).
///
/// \code
///     auto r = Peer::client()
///         .max_connections(1)
///         .connect("127.0.0.1", 60000)     // records the target
///         .start();                         // Startup, then Connect
/// \endcode
/// If no .connect() target is set, start() only performs Startup (useful for a
/// peer that will Connect() later or only Ping/Advertise). Header-only.
class ClientBuilder {
public:
    /// Maximum simultaneous connections (a pure client is typically 1).
    ClientBuilder& max_connections(unsigned int n) { maxConnections_ = n; return *this; }

    /// Optional local port to bind the client socket to. Default: OS-assigned.
    ClientBuilder& port(unsigned short p) { port_ = p; return *this; }

    /// Optional local address to bind to. Default: INADDR_ANY.
    ClientBuilder& host(const char* addr) { host_ = addr ? addr : ""; return *this; }

    /// \brief Record the connection target used by start().
    /// \param[in] host Dotted IP or domain name of the server.
    /// \param[in] remotePort Server port.
    /// \param[in] password Optional password matching the server's incoming password.
    ClientBuilder& connect(const char* host, unsigned short remotePort, const char* password = nullptr) {
        connectHost_ = host ? host : "";
        connectPort_ = remotePort;
        connectPassword_ = password ? password : "";
        wantConnect_ = true;
        return *this;
    }

    /// \brief Opt-in: public key of the secure server to connect to.
    /// \details Passed straight to Connect(); leave unset for an insecure
    /// connection (the default). The pointer must outlive start().
    ClientBuilder& public_key(PublicKey* key) { publicKey_ = key; return *this; }

    /// \brief Start up and, if a target was given, begin connecting.
    /// \return Result carrying the live Peer on success. On failure the
    /// PeerError names the stage: PeerStage::Startup carries the StartupResult,
    /// PeerStage::Connect carries the ConnectionAttemptResult.
    Result<Peer> start() {
        Peer peer;
        SocketDescriptor sd(port_, host_.empty() ? nullptr : host_.c_str());
        StartupResult sr = peer->Startup(maxConnections_, &sd, 1);
        if (sr != RAKNET_STARTED) {
            PeerError e;
            e.stage = PeerStage::Startup;
            e.startup = sr;
            return Result<Peer>::err(e);
        }
        if (wantConnect_) {
            ConnectionAttemptResult car = peer->Connect(
                connectHost_.c_str(), connectPort_,
                connectPassword_.empty() ? nullptr : connectPassword_.c_str(),
                connectPassword_.empty() ? 0 : (int) connectPassword_.size(),
                publicKey_);
            if (car != CONNECTION_ATTEMPT_STARTED) {
                PeerError e;
                e.stage = PeerStage::Connect;
                e.connect = car;
                return Result<Peer>::err(e);
            }
        }
        return Result<Peer>::ok(std::move(peer));
    }

private:
    unsigned short port_ = 0;
    unsigned int maxConnections_ = 1;
    std::string host_;
    std::string connectHost_;
    unsigned short connectPort_ = 0;
    std::string connectPassword_;
    bool wantConnect_ = false;
    PublicKey* publicKey_ = nullptr;
};

inline ServerBuilder Peer::server() { return ServerBuilder(); }
inline ClientBuilder Peer::client() { return ClientBuilder(); }

} // namespace MafiaNet
