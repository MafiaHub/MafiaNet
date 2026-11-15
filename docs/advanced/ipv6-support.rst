IPv6 Support
============

IPv6 is always compiled in. There is no build option and no preprocessor
define: the old ``RAKNET_SUPPORT_IPV6`` switch was removed, and defining it as
``0`` is a compile error. This guide covers sockets, addresses, connecting and
the wire format.

Dual-Stack Sockets (the Default)
--------------------------------

Since 0.27.0, ``SocketDescriptor::socketFamily`` defaults to ``AF_UNSPEC``: a
plain ``SocketDescriptor`` opens one dual-stack socket (``AF_INET6`` with
``IPV6_V6ONLY=0``) that serves IPv4 and IPv6 peers alike, as a wildcard listen
does in GameNetworkingSockets or ENet.

**Dual-stack server, one socket (IPv4 + IPv6):**

.. code-block:: cpp

   MafiaNet::SocketDescriptor sd(60000, nullptr); // socketFamily == AF_UNSPEC

   peer->Startup(100, &sd, 1);
   peer->SetMaximumIncomingConnections(100);

``AF_UNSPEC`` binds the wildcard address on a single dual-stack socket. IPv4
clients are reported as plain IPv4 addresses (``127.0.0.1``, never
``::ffff:127.0.0.1``), so ban lists, ``IsLoopback()`` and address lookups work
the same as on an ``AF_INET`` socket. If the host has no IPv6, or refuses
dual-stack sockets (OpenBSD), the socket falls back to IPv4 only;
``RakNetSocket2::IsDualStack()`` reports which one you got.

* ``AF_UNSPEC`` with a specific ``hostAddress`` is **not** dual-stack: it binds
  IPv4 when the address or name has an IPv4 form, and IPv6 (IPv6-only)
  otherwise. So ``"localhost"`` or a name with both A and AAAA records binds
  its IPv4 address, reachable by IPv4 clients as before; ``"::1"`` or an
  AAAA-only name binds IPv6. Dual-stack needs the wildcard address (leave
  ``hostAddress`` empty, or ``"::"``); ``"0.0.0.0"`` means "any IPv4
  address" and binds an IPv4-only socket. An IPv4-mapped bind address
  (``::ffff:a.b.c.d``) is not supported.
* ``AF_INET`` is an IPv4-only socket -- the default before 0.27.0; set it to
  get the old behaviour back. ``AF_INET6`` is an IPv6-only socket
  (``IPV6_V6ONLY=1``).

**Bound address.** ``GetMyBoundAddress()`` (and
``RakNetSocket2::GetBoundAddress()``) on a dual-stack wildcard socket reports
``127.0.0.1`` and the bound port, exactly like an IPv4 wildcard socket, so code
that connects to or compares with the bound address keeps working. Use
``IsDualStack()`` to tell the two apart. An explicit ``AF_INET6`` wildcard socket
reports ``::1``.

**Requiring dual-stack.** Set ``SocketDescriptor::requireDualStack`` to make
``Startup()`` fail with ``SOCKET_FAMILY_NOT_SUPPORTED`` instead of falling back
to IPv4 when no dual-stack socket can be opened: no IPv6 on the host,
``IPV6_V6ONLY=0`` refused, or a specific ``hostAddress`` (which is never
dual-stack). It is ignored for ``AF_INET`` and ``AF_INET6``. This is the
equivalent of GameNetworkingSockets' ``k_nAddressFamily_DualStack``.

.. code-block:: cpp

   MafiaNet::SocketDescriptor sd(60000, nullptr);
   sd.requireDualStack = true;
   if (peer->Startup(100, &sd, 1) == MafiaNet::SOCKET_FAMILY_NOT_SUPPORTED) {
       // this host cannot serve IPv4 and IPv6 on one socket
   }

.. note::

   On Linux, IPv4 and IPv6 share one UDP port space: a default (``AF_UNSPEC``)
   socket on port P cannot coexist with a separate ``AF_INET`` socket on port
   P. Code that used to open a default socket and an ``AF_INET6`` socket on the
   same port must now set ``AF_INET`` on the first one explicitly, as below;
   explicit ``AF_INET6`` sockets are IPv6-only, so that two-socket setup keeps
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

``Connect()`` resolves the host only in the families its socket reaches: on an
IPv4-only socket an IPv6 literal such as ``2001:db8::1`` (or a name with only
IPv6 addresses) does not resolve, and ``Connect()`` returns
``CANNOT_RESOLVE_DOMAIN_NAME`` at once; the same goes for IPv4 on an IPv6-only
socket. The loopback literals are the exception: ``::1`` on an IPv4-only socket
means ``127.0.0.1``, and ``127.0.0.1`` on an IPv6-only socket means ``::1``.

``SOCKET_FAMILY_NOT_SUPPORTED`` is a ``Startup()`` result, never a
``Connect()`` one: it reports a socket family the host cannot open (for example
``AF_INET6`` without IPv6), or ``requireDualStack`` that cannot be met. It
replaces the former ``BR_REQUIRES_RAKNET_SUPPORT_IPV6_DEFINE`` result, which no
longer exists.

**Happy Eyeballs (RFC 8305).** A hostname ``Connect()`` on a dual-stack socket
races the IPv6 and IPv4 candidates: attempts start 250 ms apart, interleaved by
family starting with the family of ``getaddrinfo``'s first answer (RFC 6724
ordering), and the first to complete the handshake wins. Details:

* Literal addresses are not raced; they connect directly.
* One-shot unconnected sends (``Ping``, ``AdvertiseSystem``, ``SendOutOfBand``,
  ``SendTTL``) are not raced either: on a dual-stack socket a host name goes to
  its first IPv4 address, and only a name without one goes to IPv6.
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
       char ip[MafiaNet::SystemAddress::TO_STRING_BUFFER_SIZE];
       addr.ToString(false, ip, sizeof(ip));
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
peers interoperate. This encoding was introduced in protocol version 8 and is unchanged
in the current version, 9 (``RAKNET_PROTOCOL_VERSION``); peers on a different
version are answered with ``ID_INCOMPATIBLE_PROTOCOL_VERSION``.

Platform Notes
--------------

* **SendTTL on dual-stack sockets:** ``SendTTL`` (the TTL probe used by NAT
  punchthrough) sets ``IP_TTL`` for an IPv4 target where the IPv6 socket
  accepts it, and otherwise ``IPV6_UNICAST_HOPS``; IPv6 targets always use
  ``IPV6_UNICAST_HOPS``. The old value is restored after the send. Measured:
  on Linux, ``IP_TTL`` governs the IPv4 datagrams of a dual-stack socket (the
  hop limit does not); on macOS, ``IP_TTL`` is rejected (``EINVAL``) and
  ``IPV6_UNICAST_HOPS`` governs IPv4-mapped datagrams. The integration tests
  check the TTL that actually arrives on both. Windows dual-mode sockets are
  documented to honour ``IP_TTL`` but were not measured; the BSDs share the
  macOS code but were not measured either.
* **Firewalls:** open the UDP port for IPv6 as well as IPv4.
* **NAT:** IPv6 normally needs no NAT, but NAT64 may be in the path.

Configuration Options
---------------------

.. code-block:: cpp

   SocketDescriptor sd;
   sd.socketFamily = AF_UNSPEC;     // Dual-stack on the wildcard address (default)
   sd.socketFamily = AF_INET6;      // IPv6 only
   sd.socketFamily = AF_INET;       // IPv4 only (the pre-0.27.0 default)
   sd.requireDualStack = true;      // AF_UNSPEC: fail rather than fall back to IPv4

   // Bind to a specific interface (its family decides the socket family)
   strcpy(sd.hostAddress, "2001:db8::1");

See Also
--------

* :doc:`preprocessor-directives` - Build configuration
* :doc:`nat-traversal-architecture` - NAT with IPv6
* :doc:`debugging-disconnects` - Connection issues
