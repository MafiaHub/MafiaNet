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

PacketPriority
~~~~~~~~~~~~~~

.. doxygenenum:: PacketPriority

PacketReliability
~~~~~~~~~~~~~~~~~

.. doxygenenum:: PacketReliability

StartupResult
~~~~~~~~~~~~~

.. doxygenenum:: MafiaNet::StartupResult

ConnectionAttemptResult
~~~~~~~~~~~~~~~~~~~~~~~

.. doxygenenum:: MafiaNet::ConnectionAttemptResult
