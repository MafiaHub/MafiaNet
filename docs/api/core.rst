Core API Reference
==================

This section documents the core MafiaNet classes.

RakPeerInterface
----------------

The main entry point for all networking operations.

.. doxygenclass:: MafiaNet::RakPeerInterface
   :members:
   :undoc-members:

BitStream
---------

Binary serialization for network messages.

.. doxygenclass:: MafiaNet::BitStream
   :members:
   :undoc-members:

Packet
------

Represents received network data.

.. doxygenstruct:: MafiaNet::Packet
   :members:
   :undoc-members:

SystemAddress
-------------

Network endpoint identifier (IP + port).

The ``SystemAddress`` struct contains:

- ``address`` - Union containing IPv4 (``addr4``) or IPv6 (``addr6``) address
- ``debugPort`` - Port number for debugging
- ``systemIndex`` - Internal system index

Key methods:

- ``ToString()`` - Convert to string representation
- ``FromString()`` / ``FromStringExplicitPort()`` - Parse from string
- ``GetPort()`` / ``SetPortHostOrder()`` - Port manipulation
- ``IsLoopback()`` / ``IsLANAddress()`` - Address type checks

RakNetGUID
----------

Unique identifier for each peer.

.. doxygenstruct:: MafiaNet::RakNetGUID
   :members:
   :undoc-members:

.. note::
   The non-thread-safe ``RakNetGUID::ToString(void)`` member (which returned a
   shared static buffer) was removed in 0.10.0. Use the thread-safe value-type
   helper ``MafiaNet::to_string(const RakNetGUID&)`` instead — see
   :ref:`value-type-helpers` below.

SocketDescriptor
----------------

Configuration for network sockets.

.. doxygenstruct:: MafiaNet::SocketDescriptor
   :members:
   :undoc-members:

Message Identifiers
-------------------

Built-in message types are defined in ``MessageIdentifiers.h``.

.. doxygenfile:: MessageIdentifiers.h
   :sections: enum

Enumerations
------------

Priority
~~~~~~~~

Scoped enum class ``MafiaNet::Priority`` (``Immediate``, ``High``, ``Medium``,
``Low``). Replaces the removed unscoped ``PacketPriority`` C enum; enumerator
order — and therefore the underlying integer values — is unchanged.

.. doxygenenum:: MafiaNet::Priority

Reliability
~~~~~~~~~~~

Scoped enum class ``MafiaNet::Reliability`` (``Unreliable``,
``UnreliableSequenced``, ``Reliable``, ``ReliableOrdered``,
``ReliableSequenced``, ``UnreliableWithAckReceipt``, ``ReliableWithAckReceipt``,
``ReliableOrderedWithAckReceipt``). Replaces the removed unscoped
``PacketReliability`` C enum; enumerator order is preserved, so the 3-bit
reliability wire field stays compatible.

.. doxygenenum:: MafiaNet::Reliability

StartupResult
~~~~~~~~~~~~~

.. doxygenenum:: MafiaNet::StartupResult

ConnectionAttemptResult
~~~~~~~~~~~~~~~~~~~~~~~

.. doxygenenum:: MafiaNet::ConnectionAttemptResult

Umbrella Header
---------------

``#include "mafianet/mafianet.h"`` pulls in the core public API — the peer
interface, types, message IDs, ``Priority`` / ``Reliability``, ``BitStream``,
the serialization archives, ``GetTime``, statistics, the canonical aliases, the
RAII handles, and the typed message dispatcher — behind a single include, so the
common client/server path needs only one ``#include``:

.. code-block:: cpp

   #include "mafianet/mafianet.h"

It is purely additive: the granular per-feature headers remain for advanced
users. Encryption headers are intentionally omitted — connection security stays
opt-in via ``RakPeerInterface::InitializeSecurity()`` (see
:doc:`../basics/secure-connections`).

Canonical Type Aliases
----------------------

``mafianet/aliases.h`` (also pulled in by the umbrella header) provides
canonical MafiaNet names over the legacy RakNet-named public types. They are
``using`` aliases denoting the *exact same* types/objects, so old and new names
interoperate freely:

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Canonical alias
     - Legacy name
   * - ``MafiaNet::PeerInterface``
     - ``RakPeerInterface``
   * - ``MafiaNet::Guid``
     - ``RakNetGUID``
   * - ``MafiaNet::Statistics``
     - ``RakNetStatistics``
   * - ``MafiaNet::UnassignedGuid``
     - ``UNASSIGNED_RAKNET_GUID``

RAII Handles (Peer / PacketPtr)
-------------------------------

``mafianet/PeerHandle.h`` (exported from the umbrella header) provides two RAII
owners that remove manual ``DestroyInstance`` / ``DeallocatePacket``
bookkeeping:

* ``MafiaNet::Peer`` owns a ``RakPeerInterface`` instance and destroys it on
  scope exit.
* ``MafiaNet::PacketPtr`` owns a received ``Packet`` and deallocates it
  automatically.

.. code-block:: cpp

   #include "mafianet/mafianet.h"

   MafiaNet::Peer peer;                         // owns a RakPeerInterface
   MafiaNet::SocketDescriptor sd(60000, 0);
   peer->Startup(8, &sd, 1);                    // operator-> forwards to the interface

   while (running) {
       MafiaNet::PacketPtr packet = peer.receive();   // owns the Packet
       if (!packet) continue;
       switch (packet->data[0]) {
           // ... handle message ...
       }
       // no DeallocatePacket / DestroyInstance — destructors handle it
   }

``Peer::incoming()`` wraps the drain loop in a single-pass input range. Each
iteration yields a fresh ``PacketPtr`` that is deallocated when the loop body
scope ends, so the queue is drained without any manual bookkeeping:

.. code-block:: cpp

   for (auto packet : peer.incoming()) {   // packet frees at end of each iteration
       switch (packet.id()) {              // ID_TIMESTAMP-aware identifier
           // ... handle message ...
       }
   }

Startup Builders (Peer::server / Peer::client)
----------------------------------------------

``Peer::server()`` and ``Peer::client()`` return fluent builders that fold the
multi-call startup dance (construct a ``SocketDescriptor``, ``Startup``, check
``== RAKNET_STARTED``, ``SetMaximumIncomingConnections`` / ``Connect``) into a
single chain. The builder retains the socket configuration (port, bind address)
and constructs the ``SocketDescriptor`` during ``start()``, so the caller never
manages the array or its count.

``start()`` returns a ``MafiaNet::Result<Peer>``: test it with ``if (result)``,
reach the live ``Peer`` with ``*result`` / ``result.value()``, and on failure
read ``result.error()`` — which preserves the underlying engine enum rather than
collapsing it to a bool.

.. code-block:: cpp

   auto server = MafiaNet::Peer::server()
       .port(60000)
       .max_connections(32)
       .incoming_password("hunter2")      // optional -> SetIncomingPassword
       .start();                          // Startup + SetMaximumIncomingConnections
   if (!server) {
       printf("startup failed: %d\n", (int) server.error().startup);   // StartupResult
       return;
   }

   auto client = MafiaNet::Peer::client()
       .max_connections(1)
       .connect("127.0.0.1", 60000)       // records the target
       .start();                          // Startup, then Connect
   if (!client) {
       // error().stage is PeerStage::Startup or PeerStage::Connect; the matching
       // member (error().startup / error().connect) carries the engine enum.
       return;
   }

   MafiaNet::Peer peer = std::move(*client);   // Result<Peer> is move-only

Security is opt-in and unchanged from the raw API: ``ServerBuilder::secure(publicKey,
privateKey)`` calls ``InitializeSecurity`` before ``Startup`` (only when
``LIBCAT_SECURITY==1``), and ``ClientBuilder::public_key()`` forwards a
``PublicKey*`` to ``Connect``. Encryption is never implied. The builder stores
these as raw pointers, so the key buffers passed to ``secure()`` and the
``PublicKey`` passed to ``public_key()`` must remain valid until ``start()``
returns.

Serialization Archives (WriteArchive / ReadArchive)
---------------------------------------------------

``mafianet/Archive.h`` (pulled in by the umbrella header) provides a single
serialization convention over ``BitStream``. A user type describes its wire
format once with a member template, and the same ``serialize()`` serves both
directions — no duplicated read/write field lists:

.. code-block:: cpp

   struct ChatMessage {
       std::string text;
       int channel;
       template <class Ar> void serialize(Ar& ar) { ar & text & channel; }
   };

   MafiaNet::BitStream bs;
   ChatMessage msg{ "hi", 3 };
   MafiaNet::WriteArchive(bs) & msg;          // write

   MafiaNet::BitStream in(bs.GetData(), bs.GetNumberOfBytesUsed(), false);
   ChatMessage got;
   MafiaNet::ReadArchive(in) & got;           // read; got == msg

The archives reuse ``BitStream``'s existing ``operator<<`` / ``operator>>`` and
its per-type specializations (``std::string``, ``SystemAddress``,
``RakNetGUID``, ...), so primitives are never reimplemented. A field whose type
has its own ``serialize()`` recurses through the archive; any other field falls
through to ``bs << field`` / ``bs >> field``, which also picks up a
user-provided shift operator for that type.

A ``serialize()`` can branch on direction via the ``Ar::IsWriting`` /
``Ar::IsReading`` compile-time constants, and ``ar.stream()`` exposes the
underlying ``BitStream`` (e.g. to write a header before the body).

This header is *only* the wire convention — message-ID assignment and dispatch
are the :ref:`Dispatcher <typed-dispatcher>`'s job.

.. _typed-dispatcher:

Typed Message Dispatcher (Dispatcher / Sender)
----------------------------------------------

``mafianet/Dispatcher.h`` (pulled in by the umbrella header) replaces the
classic giant ``switch`` on ``packet->data[0]`` — plus the hand-rolled
``ID_TIMESTAMP``-skipping helper — with typed handlers. It builds on the
receive iterator, ``PacketPtr``, and the serialization archives:

.. code-block:: cpp

   MafiaNet::Dispatcher d;
   d.on<ChatMessage>([](const ChatMessage& m, const MafiaNet::Sender& from) {
       printf("%s says: %s\n", from.guid_string().c_str(), m.text.c_str());
   });
   d.on(ID_NEW_INCOMING_CONNECTION, [](const MafiaNet::Sender& s) { /* ... */ });

   for (auto pkt : peer.incoming())
       d.dispatch(pkt);

A typed handler for ``T`` receives a freshly deserialized ``T`` (its
``serialize()`` run in read mode over the packet body) plus a ``Sender``; a
system handler receives only a ``Sender``. ``Sender`` is a lightweight,
non-owning view of the packet's identity fields — ``guid()``, ``peer_guid()``,
``address()``, ``guid_string()``, and ``packet()`` for handlers that need more
— valid only for the duration of the handler call.

``dispatch()`` returns ``false`` for an unregistered identifier (or an empty
packet), leaving the caller free to fall back to a ``switch``; it transparently
skips an ``ID_TIMESTAMP`` + ``Time`` prefix either way. ``encode(bitStream,
message)`` is the symmetric write path: identifier byte followed by the
archived body, ready to ``Send()`` (or use the typed ``Peer::send`` /
``Peer::broadcast`` below, which call it for you).

.. important:: **Message-ID contract.** ``on<T>(handler)`` auto-assigns the next
   free identifier starting at ``ID_USER_PACKET_ENUM``, in registration order —
   **both peers must register their typed messages in the same order** for the
   identifiers to line up on the wire. When that is inconvenient (plugins,
   conditional registration, cross-language peers), pass an explicit identifier
   with ``on<T>(id, handler)`` and the order no longer matters. The assigned
   identifier is returned from ``on<T>()`` and queryable via ``id_of<T>()`` /
   ``has<T>()``.

The dispatcher is opt-in sugar: ``pkt.id()`` and a raw ``switch`` remain fully
usable, and nothing changes about how packets are received. It is not
thread-safe — build the handler table up front, then dispatch from the single
thread that drains the receive queue.

Typed Send / Broadcast (Peer::send / Peer::broadcast)
-----------------------------------------------------

``Peer::send<T>()`` and ``Peer::broadcast<T>()`` are the typed counterparts of
the 8-argument ``Send()``. Each encodes the message identifier plus the
archived body via the dispatcher's registry into a scratch ``BitStream`` and
forwards to ``Send(const BitStream*, ...)``, with overridable defaults
(``Priority::High``, ``Reliability::ReliableOrdered``, ordering channel 0):

.. code-block:: cpp

   peer.send(dispatcher, ChatMessage{"hi", 0}, to);                        // defaults
   peer.send(dispatcher, ChatMessage{"hi", 0}, to, Reliability::Unreliable);
   peer.broadcast(dispatcher, ChatMessage{"hi", 0});                       // everyone

``to`` accepts an ``AddressOrGUID`` (implicitly built from a ``SystemAddress``
or ``RakNetGUID``); ``broadcast()`` sets the broadcast flag with no exclusion
target, so every connected system receives the message. Both return the
``Send()`` receipt. ``T`` must be registered with the dispatcher (its
identifier must be known), and the raw ``Send()`` remains untouched.

.. _value-type-helpers:

Value-Type GUID Helpers
-----------------------

``mafianet/guid_util.h`` provides modern, thread-safe value-type accessors:

* ``std::string MafiaNet::to_string(const RakNetGUID&)`` — owns its buffer and
  is thread-safe (replaces the removed ``RakNetGUID::ToString(void)``).
* ``std::optional<SystemAddress> MafiaNet::connected_address(RakPeerInterface&,
  const RakNetGUID&)`` — maps the ``UNASSIGNED_SYSTEM_ADDRESS`` sentinel to
  ``std::nullopt``.

.. code-block:: cpp

   printf("Peer %s connected\n", MafiaNet::to_string(packet->guid).c_str());

   if (auto addr = MafiaNet::connected_address(*peer, guid))
       printf("address: %s\n", addr->ToString());
