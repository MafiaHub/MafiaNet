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
``GetTime``, statistics, the canonical aliases, and the RAII handles — behind a
single include, so the common client/server path needs only one ``#include``:

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
