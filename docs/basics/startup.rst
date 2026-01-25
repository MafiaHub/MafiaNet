Startup
=======

The first step to use MafiaNet is calling ``RakPeerInterface::Startup()``.

Starting RakPeerInterface
-------------------------

.. code-block:: cpp

   StartupResult RakPeer::Startup(
       unsigned short maxConnections,
       SocketDescriptor *socketDescriptors,
       unsigned socketDescriptorCount,
       int threadPriority = -99999
   );

``Startup()`` will:

1. Generate ``RakNetGUID``, a unique identifier for this instance
2. Allocate an array of reliable connection slots (defined by ``maxConnections``)
3. Create one or more sockets (described by ``socketDescriptors``)

Before calling ``Startup()``, only raw UDP functions are available: ``Ping()``, ``AdvertiseSystem()``, and ``SendOutOfBand()``.

Parameters
----------

maxConnections
~~~~~~~~~~~~~~

MafiaNet preallocates memory for connections. Specify the maximum supported number of connections (both incoming and outgoing). If you want other systems to connect to you, also call ``SetMaximumIncomingConnections()`` with a value ≤ maxConnections.

socketDescriptors
~~~~~~~~~~~~~~~~~

In 95% of cases, use:

.. code-block:: cpp

   MafiaNet::SocketDescriptor sd(MY_LOCAL_PORT, 0);

For **servers/peers**: Set the port to your desired server port. This is the ``remotePort`` parameter clients will use with ``Connect()``.

For **clients**: Use port 0 to automatically pick a free port.

.. note::
   On Linux, ports ≤ 1000 require admin rights.

Multiple Socket Bindings
~~~~~~~~~~~~~~~~~~~~~~~~

For advanced users binding multiple network cards:

.. code-block:: cpp

   MafiaNet::SocketDescriptor sdArray[2];
   sdArray[0].port = SERVER_PORT_1;
   strcpy(sdArray[0].hostAddress, "192.168.0.1");
   sdArray[1].port = SERVER_PORT_2;
   strcpy(sdArray[1].hostAddress, "192.168.0.2");

   if (rakPeer->Startup(32, sdArray, 2) == MafiaNet::RAKNET_STARTED) {
       OnRakNetStarted();
   }

IPv6 Support
~~~~~~~~~~~~

IPv6 uses 16-byte addresses (e.g., ``fe80::7c:31f7:fec4:27de%14``) instead of 4-byte IPv4 addresses. NAT Punchthrough is not needed with IPv6.

To enable IPv6:

.. code-block:: cpp

   socketDescriptor.socketFamily = AF_INET6;

.. warning::
   IPv6 sockets can only connect to other IPv6 sockets. IPv4 sockets can only connect to IPv4.

threadPriority
~~~~~~~~~~~~~~

- **Windows**: Priority passed to ``_beginthreadex()``. Default (-99999) uses NORMAL_PRIORITY.
- **Linux**: Passed to ``pthread_attr_setschedparam()``. Default uses priority 1000.

Example: Basic Server Startup
-----------------------------

.. code-block:: cpp

   #include "mafianet/peerinterface.h"

   MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();

   MafiaNet::SocketDescriptor sd(60000, 0);  // Port 60000

   MafiaNet::StartupResult result = server->Startup(100, &sd, 1);
   if (result != MafiaNet::RAKNET_STARTED) {
       printf("Failed to start: %d\n", result);
       return -1;
   }

   // Allow incoming connections
   server->SetMaximumIncomingConnections(100);

Example: Basic Client Startup
-----------------------------

.. code-block:: cpp

   MafiaNet::RakPeerInterface* client = MafiaNet::RakPeerInterface::GetInstance();

   MafiaNet::SocketDescriptor sd;  // Port 0 = auto-select
   client->Startup(1, &sd, 1);     // Only need 1 connection slot

Getting Your GUID
-----------------

After startup, get your unique identifier:

.. code-block:: cpp

   MafiaNet::RakNetGUID myGuid = peer->GetGuidFromSystemAddress(
       MafiaNet::UNASSIGNED_SYSTEM_ADDRESS
   );

See Also
--------

* :doc:`connecting` - How to connect to other systems
* :doc:`../getting-started/building` - Build configuration
