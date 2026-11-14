IPv6 Support
============

MafiaNet supports IPv6 networking alongside IPv4. This guide covers enabling,
configuring, and using IPv6 connections.

Enabling IPv6
-------------

IPv6 must be enabled at compile time:

.. code-block:: cpp

   // In RakNetDefinesOverrides.h
   #define RAKNET_SUPPORT_IPV6 1

Then rebuild the library.

Basic Usage
-----------

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
``RakNetSocket2::IsDualStack()`` reports which one you got. A host name passed
to ``Connect()`` on a dual-stack socket resolves to the resolver's first answer
of either family.

.. note::

   On Linux, IPv4 and IPv6 share one UDP port space: an ``AF_UNSPEC`` socket on
   port P cannot coexist with an ``AF_INET`` socket on port P. Explicit
   ``AF_INET6`` sockets are IPv6-only (``IPV6_V6ONLY=1``), so the two-socket
   setup below keeps working.

**Dual-stack server, two sockets (IPv4 + IPv6):**

.. code-block:: cpp

   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   // Create socket descriptors for both protocols
   MafiaNet::SocketDescriptor sockets[2];

   // IPv4 socket
   sockets[0].port = 60000;
   sockets[0].socketFamily = AF_INET;

   // IPv6 socket
   sockets[1].port = 60000;
   sockets[1].socketFamily = AF_INET6;

   // Start with both sockets
   peer->Startup(100, sockets, 2);
   peer->SetMaximumIncomingConnections(100);

**IPv6-only server:**

.. code-block:: cpp

   MafiaNet::SocketDescriptor sd;
   sd.port = 60000;
   sd.socketFamily = AF_INET6;

   peer->Startup(100, &sd, 1);

**Connecting to IPv6 address:**

.. code-block:: cpp

   // Connect using IPv6 address
   peer->Connect("2001:db8::1", 60000, nullptr, 0);

   // Connect using hostname (resolves to IPv4 or IPv6)
   peer->Connect("game.example.com", 60000, nullptr, 0);

**Checking address type:**

.. code-block:: cpp

   MafiaNet::SystemAddress addr = packet->systemAddress;

   if (addr.IsIPv6()) {
       char ip[INET6_ADDRSTRLEN];
       addr.ToString(false, ip);
       printf("IPv6 client: %s\n", ip);
   } else {
       printf("IPv4 client: %s\n", addr.ToString(false));
   }

Key Considerations
------------------

* **Dual-stack**: Most modern systems support dual-stack networking
* **Firewall**: Ensure IPv6 ports are open in firewall rules
* **NAT**: IPv6 typically doesn't require NAT (but may use NAT64)
* **DNS**: Use hostnames when possible for automatic protocol selection

Address Formats
---------------

Valid IPv6 address formats:

* Full: ``2001:0db8:0000:0000:0000:0000:0000:0001``
* Compressed: ``2001:db8::1``
* IPv4-mapped: ``::ffff:192.168.1.1``
* Link-local: ``fe80::1%eth0``

Configuration Options
---------------------

.. code-block:: cpp

   // Socket descriptor options
   SocketDescriptor sd;
   sd.socketFamily = AF_INET6;      // IPv6 only
   sd.socketFamily = AF_INET;       // IPv4 only
   sd.socketFamily = AF_UNSPEC;     // Dual-stack (falls back to IPv4)

   // Bind to specific interface
   strcpy(sd.hostAddress, "2001:db8::1");

See Also
--------

* :doc:`preprocessor-directives` - Build configuration
* :doc:`nat-traversal-architecture` - NAT with IPv6
* :doc:`debugging-disconnects` - Connection issues
