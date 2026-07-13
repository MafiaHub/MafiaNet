/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file Dispatcher.h
/// \brief A typed message dispatcher over the receive iterator (issue #20).
///
/// \details The classic RakNet receive loop is a giant `switch` on the first
/// byte, plus a hand-rolled helper to skip the `ID_TIMESTAMP` prefix. The
/// dispatcher replaces that boilerplate with typed handlers, building on the
/// receive iterator (#17), the RAII PacketPtr (#16) and the serialization
/// archive (#19):
/// \code
/// struct ChatMessage {
///     std::string text; int channel;
///     template <class Ar> void serialize(Ar& ar) { ar & text & channel; }
/// };
///
/// MafiaNet::Dispatcher d;
/// d.on<ChatMessage>([](const ChatMessage& m, const MafiaNet::Sender& from) {
///     printf("%s says: %s\n", from.guid_string().c_str(), m.text.c_str());
/// });
/// d.on(ID_NEW_INCOMING_CONNECTION, [](const MafiaNet::Sender& s) { /* ... */ });
///
/// for (auto pkt : peer.incoming())
///     d.dispatch(pkt);
/// \endcode
///
/// \par Message-ID contract
/// `on<T>()` assigns the next free identifier starting at `ID_USER_PACKET_ENUM`,
/// in registration order. **Both peers must register their typed messages in the
/// same order** for the identifiers to line up on the wire — the mirror of how
/// hand-written code hard-codes `ID_USER_PACKET_ENUM + N`. When that is
/// inconvenient (plugins, conditional registration, cross-language peers), pass
/// an explicit identifier with `on<T>(id, handler)` and the order no longer
/// matters. System identifiers (below `ID_USER_PACKET_ENUM`) are always explicit
/// via `on(id, handler)`.
///
/// The dispatcher is opt-in sugar: `pkt.id()` and a raw `switch` remain fully
/// usable, and nothing here changes how packets are received.

#ifndef __MAFIANET_DISPATCHER_H
#define __MAFIANET_DISPATCHER_H

#include <functional>    // std::function
#include <string>        // std::string (Sender::guid_string)
#include <typeindex>     // std::type_index (T -> id registry)
#include <unordered_map> // handler / id tables

#include "mafianet/Archive.h"            // ReadArchive / WriteArchive
#include "mafianet/BitStream.h"          // BitStream over the packet body
#include "mafianet/MessageIdentifiers.h" // ID_TIMESTAMP, ID_USER_PACKET_ENUM
#include "mafianet/PeerHandle.h"         // PacketPtr
#include "mafianet/guid_util.h"          // MafiaNet::to_string (owning, thread-safe)
#include "mafianet/time.h"               // MafiaNet::Time (timestamp-prefix width)
#include "mafianet/types.h"              // Packet, MessageID, RakNetGUID, SystemAddress, PeerGuid

namespace MafiaNet {

/// \brief Lightweight, non-owning view of who a dispatched packet came from.
///
/// Wraps the identity fields of the received Packet so handlers can answer "who
/// sent this?" without touching the raw Packet. It borrows the packet — valid
/// only for the duration of the handler call — and copies nothing.
class Sender {
public:
    explicit Sender(const Packet& packet) : packet_(&packet) {}

    /// The stable GUID of the sender (UNASSIGNED_RAKNET_GUID before a connection
    /// is established, e.g. on offline/advertise packets).
    const RakNetGUID& guid() const { return packet_->guid; }

    /// The sender's GUID as a strong, wire-compatible scoped enum (see PeerGuid).
    PeerGuid peer_guid() const { return ToPeerGuid(packet_->guid); }

    /// The sender's network address (IP + port).
    const SystemAddress& address() const { return packet_->systemAddress; }

    /// The GUID as an owned, thread-safe std::string (via MafiaNet::to_string, #15).
    std::string guid_string() const { return to_string(packet_->guid); }

    /// The underlying packet, for handlers that need more than identity.
    const Packet& packet() const { return *packet_; }

private:
    const Packet* packet_;
};

/// \brief Routes received packets to typed or per-identifier handlers (issue #20).
///
/// Register handlers once, then feed every received packet to dispatch(). A
/// typed handler for `T` receives a freshly deserialized `T` (its serialize()
/// run in read mode over the packet body) plus a \ref Sender; a system handler
/// receives only a \ref Sender. See the header comment for the ID contract.
///
/// Not thread-safe: build the handler table up front, then dispatch from the
/// single thread that drains the receive queue.
class Dispatcher {
public:
    /// Handler for a typed message body plus its sender.
    template <class T>
    using TypedHandler = std::function<void(const T&, const Sender&)>;

    /// Handler for a bare identifier (system messages, or payloads you parse yourself).
    using SystemHandler = std::function<void(const Sender&)>;

    /// \brief Register \a handler for \a T, auto-assigning the next identifier.
    /// \return The assigned identifier (use it when sending, or query id_of<T>()).
    /// \note Assigns identifiers in registration order from ID_USER_PACKET_ENUM;
    /// peers must register in the same order (see the header ID contract).
    template <class T>
    MessageID on(TypedHandler<T> handler) {
        return on<T>(nextAutoId(), std::move(handler));
    }

    /// \brief Register \a handler for \a T under an explicit \a id.
    /// \details Bypasses registration-order coupling — both peers just agree on
    /// \a id. \a id may be a user identifier (>= ID_USER_PACKET_ENUM) or, if you
    /// really want a struct behind a system id, anything else.
    /// \return \a id, for symmetry with the auto-assigning overload.
    template <class T>
    MessageID on(MessageID id, TypedHandler<T> handler) {
        typeIds_[std::type_index(typeid(T))] = id;
        handlers_[id] = [handler = std::move(handler)](const Packet& packet, unsigned int bodyOffset) {
            // A read-only BitStream over the message body (everything after the
            // identifier byte, and after any ID_TIMESTAMP + Time prefix). copyData
            // is false: the stream borrows the packet buffer for this call only.
            BitStream body(packet.data + bodyOffset, packet.length - bodyOffset, false);
            T message;
            ReadArchive(body) & message;
            handler(message, Sender(packet));
        };
        return id;
    }

    /// \brief Register \a handler for a bare identifier \a id.
    /// \details Use for engine notifications (ID_NEW_INCOMING_CONNECTION,
    /// ID_DISCONNECTION_NOTIFICATION, ...) or for messages you decode by hand.
    void on(MessageID id, SystemHandler handler) {
        handlers_[id] = [handler = std::move(handler)](const Packet& packet, unsigned int) {
            handler(Sender(packet));
        };
    }

    /// \brief Route one received packet to its handler, if any is registered.
    /// \return true if a handler ran; false for an empty packet or an
    /// unregistered identifier (leaving the caller free to fall back to a switch).
    bool dispatch(const PacketPtr& packet) const {
        if (!packet)
            return false;
        return dispatch(*packet);
    }

    /// \brief Route a raw Packet (for callers still on the legacy Receive() API).
    bool dispatch(const Packet& packet) const {
        if (packet.length == 0)
            return false;

        // Locate the identifier byte and the body that follows it, skipping an
        // ID_TIMESTAMP + Time prefix exactly like the classic GetPacketIdentifier.
        unsigned int idOffset = 0;
        if (static_cast<unsigned char>(packet.data[0]) == ID_TIMESTAMP) {
            const unsigned int prefix = sizeof(MessageID) + sizeof(Time);
            if (packet.length <= prefix)
                return false; // truncated timestamp packet: nothing to dispatch
            idOffset = prefix;
        }

        const MessageID id = static_cast<MessageID>(packet.data[idOffset]);
        const auto it = handlers_.find(id);
        if (it == handlers_.end())
            return false;

        it->second(packet, idOffset + sizeof(MessageID));
        return true;
    }

    /// \brief The identifier registered for \a T.
    /// \pre \a T was registered via on<T>(). Undefined otherwise — check with
    /// has<T>() first if unsure.
    template <class T>
    MessageID id_of() const {
        return typeIds_.at(std::type_index(typeid(T)));
    }

    /// \brief Whether a typed handler is registered for \a T.
    template <class T>
    bool has() const {
        return typeIds_.find(std::type_index(typeid(T))) != typeIds_.end();
    }

    /// \brief Encode \a message into \a bitStream ready to Send(): identifier byte
    /// followed by the archived body.
    /// \pre \a T was registered via on<T>() (its identifier must be known).
    /// \details Symmetric with dispatch(): what this writes, dispatch() reads. It
    /// only fills the BitStream — sending it over a Peer is the caller's job
    /// (see issue #21). Does not add an ID_TIMESTAMP prefix; dispatch() handles
    /// packets with or without one regardless.
    template <class T>
    void encode(BitStream& bitStream, const T& message) const {
        bitStream.Write(id_of<T>());
        WriteArchive(bitStream) & const_cast<T&>(message);
    }

private:
    /// Next unused identifier at or above ID_USER_PACKET_ENUM. Skips identifiers
    /// already taken by an explicit on<T>(id, ...) / on(id, ...) registration.
    MessageID nextAutoId() {
        while (handlers_.find(nextId_) != handlers_.end())
            ++nextId_;
        return nextId_;
    }

    // Erased handler: given the packet and the byte offset of its body, decode
    // and invoke. One entry per registered identifier.
    using ErasedHandler = std::function<void(const Packet&, unsigned int)>;

    std::unordered_map<MessageID, ErasedHandler> handlers_;
    std::unordered_map<std::type_index, MessageID> typeIds_;
    MessageID nextId_ = ID_USER_PACKET_ENUM;
};

} // namespace MafiaNet

#endif // __MAFIANET_DISPATCHER_H
