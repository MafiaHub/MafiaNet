IPv6 Support
============

IPv6 is always compiled in. There is no build option and no preprocessor
define: the old ``RAKNET_SUPPORT_IPV6`` switch was removed, and defining it as
``0`` is a compile error. This guide covers sockets, addresses, connecting and
the wire format.

Dual-Stack Sockets (AF_UNSPEC)
------------------------------

**Dual-stack server, one socket (IPv4 + IPv6):**

.. code-block:: cpp

   MafiaNet::SocketDescriptor sd;
   sd.port = 60000;
   sd.socketFamily = AF_UNSPEC; // one AF_INET6 socket with IPV6_V6ONLY=0

   peer->Startup(100, &sd, 1);
   peer->SetMaximumIncomingConnections(100);

``AF_UNSPEC`` binds the wildcard address on a single dual-stack socket. IPv4
clients are reported as plain IPv4 addresses (``127.0.0.1``, never
``::ffff:127.0.0.1``), so ban lists, ``IsLoopback()`` and address lookups work
the same as on an ``AF_INET`` socket. If the host has no IPv6, or refuses
dual-stack sockets (OpenBSD), the socket falls back to IPv4 only;
``RakNetSocket2::IsDualStack()`` reports which one you got.

* ``AF_UNSPEC`` with a specific ``hostAddress`` binds that address's own family
  and is **not** dual-stack. Dual-stack needs the wildcard address (leave
  ``hostAddress`` empty). An IPv4-mapped bind address (``::ffff:a.b.c.d``) is
  not supported.
* ``AF_INET`` is an IPv4-only socket. ``AF_INET6`` is an IPv6-only socket
  (``IPV6_V6ONLY=1``).

.. note::

   On Linux, IPv4 and IPv6 share one UDP port space: an ``AF_UNSPEC`` socket on
   port P cannot coexist with an ``AF_INET`` socket on port P. Explicit
   ``AF_INET6`` sockets are IPv6-only, so the two-socket setup below keeps
   working.

**Dual-stack server, two sockets (IPv4 + IPv6):**

.. code-block:: cpp

   MafiaNet::SocketDescriptor sockets[2];

   sockets[0].port = 60000;
   sockets[0].socketFamily = AF_INET;

   sockets[1].port = 60000;
   sockets[1].socketFamily = AF_INET6;

   peer->Startup(100, sockets, 2);
   peer->SetMaximumIncomingConnections(100);

**IPv6-only server:**

.. code-block:: cpp

   MafiaNet::SocketDescriptor sd;
   sd.port = 60000;
   sd.socketFamily = AF_INET6;

   peer->Startup(100, &sd, 1);

Connecting
----------

.. code-block:: cpp

   // IPv6 literal
   peer->Connect("2001:db8::1", 60000, nullptr, 0);

   // Hostname, resolved to IPv6 and/or IPv4 candidates
   peer->Connect("game.example.com", 60000, nullptr, 0);

Connecting to a family the socket does not have fails with
``SOCKET_FAMILY_NOT_SUPPORTED`` (the former
``BR_REQUIRES_RAKNET_SUPPORT_IPV6_DEFINE`` result no longer exists).

**Happy Eyeballs (RFC 8305).** A hostname ``Connect()`` on a dual-stack socket
races the IPv6 and IPv4 candidates: attempts start 250 ms apart, interleaved by
family starting with the family of ``getaddrinfo``'s first answer (RFC 6724
ordering), and the first to complete the handshake wins. Details:

* Literal addresses are not raced; they connect directly.
* A rejection from the server (for example a full server or a banned address)
  ends the whole race. Only silence moves on to the next candidate.
* ``CancelConnectionAttempt()`` on the first candidate's address cancels the
  whole race.
* On a threaded peer a cancel can lose to work the network thread has already
  started, so one more connection request may still go out after the call.

Addresses
---------

.. code-block:: cpp

   MafiaNet::SystemAddress addr = packet->systemAddress;

   if (addr.GetIPVersion() == 6) {
       char ip[INET6_ADDRSTRLEN];
       addr.ToString(false, ip);
       printf("IPv6 client: %s\n", ip);
   } else {
       printf("IPv4 client: %s\n", addr.ToString(false));
   }

Valid IPv6 address formats:

* Full: ``2001:0db8:0000:0000:0000:0000:0000:0001``
* Compressed: ``2001:db8::1``
* IPv4-mapped: ``::ffff:192.168.1.1``
* Link-local: ``fe80::1%eth0``

**Mapped-address normalization.** An IPv4-mapped address is always stored as
plain IPv4, whether it comes from a socket, from ``FromString()`` or from the
wire. ``SystemAddress::IsIPv4Mapped()`` and ``UnmapIPv4()`` are available for
addresses obtained elsewhere.

**Scope ids are not transmitted.** The zone suffix of a link-local address
(``%eth0``) is meaningful only on the local host, so it is not sent over the
network, and link-local addresses are not advertised as internal IDs.

Wire Format
-----------

An IPv6 ``SystemAddress`` is serialized as 19 bytes::

   [u8 6][16 bytes inverted address][u16 port, network order]

IPv4 addresses keep their 7-byte form. IPv4-mapped addresses are sent as IPv4.
The layout is the same on every platform, so Windows, Linux, macOS and BSD
peers interoperate. This encoding is part of protocol version 8
(``RAKNET_PROTOCOL_VERSION``); peers on an older version are answered with
``ID_INCOMPATIBLE_PROTOCOL_VERSION``.

Platform Notes
--------------

* **macOS and BSD dual-stack sockets:** ``SendTTL`` (the TTL probe used by NAT
  punchthrough) to an IPv4 target cannot set ``IP_TTL`` on the IPv6 socket
  (``EINVAL``), so the datagram goes out at full TTL. Linux and Windows apply
  the TTL.
* **Firewalls:** open the UDP port for IPv6 as well as IPv4.
* **NAT:** IPv6 normally needs no NAT, but NAT64 may be in the path.

Configuration Options
---------------------

.. code-block:: cpp

   SocketDescriptor sd;
   sd.socketFamily = AF_INET6;      // IPv6 only
   sd.socketFamily = AF_INET;       // IPv4 only
   sd.socketFamily = AF_UNSPEC;     // Dual-stack on the wildcard address

   // Bind to a specific interface (its family decides the socket family)
   strcpy(sd.hostAddress, "2001:db8::1");

See Also
--------

* :doc:`preprocessor-directives` - Build configuration
* :doc:`nat-traversal-architecture` - NAT with IPv6
* :doc:`debugging-disconnects` - Connection issues
