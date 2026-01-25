SystemAddress
=============

``SystemAddress`` uniquely identifies a network endpoint by IP address and port.

Structure
---------

.. code-block:: cpp

   struct SystemAddress {
       // Binary IP address (network byte order)
       union {
           unsigned char addr4[4];    // IPv4
           unsigned char addr6[16];   // IPv6
       };
       unsigned short port;           // Port (host byte order)
       // ...
   };

Creating SystemAddress
----------------------

From String
~~~~~~~~~~~

.. code-block:: cpp

   MafiaNet::SystemAddress addr;
   addr.FromString("192.168.1.100:60000");

   // Or separately
   addr.FromString("192.168.1.100");
   addr.SetPortHostOrder(60000);

From IP and Port
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   MafiaNet::SystemAddress addr("192.168.1.100", 60000);

   // Or using SetBinaryAddress
   MafiaNet::SystemAddress addr;
   addr.SetBinaryAddress("192.168.1.100");
   addr.SetPortHostOrder(60000);

Special Values
~~~~~~~~~~~~~~

.. code-block:: cpp

   // Unassigned (used for broadcast)
   MafiaNet::UNASSIGNED_SYSTEM_ADDRESS

   // Localhost
   MafiaNet::SystemAddress localhost("127.0.0.1", 60000);

Converting to String
--------------------

.. code-block:: cpp

   MafiaNet::SystemAddress addr;

   // Full address with port
   const char* str = addr.ToString(true);   // "192.168.1.100:60000"

   // IP only
   const char* ip = addr.ToString(false);   // "192.168.1.100"

   // With buffer (thread-safe)
   char buffer[64];
   addr.ToString(true, buffer);

Getting Components
------------------

.. code-block:: cpp

   // Get port
   unsigned short port = addr.GetPort();

   // Get IP as string
   const char* ip = addr.ToString(false);

   // Check if valid
   if (addr != MafiaNet::UNASSIGNED_SYSTEM_ADDRESS) {
       // Address is valid
   }

Comparison
----------

.. code-block:: cpp

   MafiaNet::SystemAddress addr1("192.168.1.100", 60000);
   MafiaNet::SystemAddress addr2("192.168.1.100", 60000);

   if (addr1 == addr2) {
       // Same address
   }

   if (addr1 != MafiaNet::UNASSIGNED_SYSTEM_ADDRESS) {
       // Valid address
   }

SystemAddress vs RakNetGUID
---------------------------

MafiaNet provides two ways to identify peers:

**SystemAddress**:

- IP + Port combination
- Changes if client reconnects from different IP/port
- Used for network operations

**RakNetGUID**:

- Unique 64-bit identifier generated at startup
- Survives reconnections (if you track it)
- Better for player identification

.. code-block:: cpp

   // Get GUID from SystemAddress
   MafiaNet::RakNetGUID guid = peer->GetGuidFromSystemAddress(addr);

   // Get SystemAddress from GUID
   MafiaNet::SystemAddress addr = peer->GetSystemAddressFromGuid(guid);

Using with Send
---------------

You can use either SystemAddress or GUID with Send:

.. code-block:: cpp

   // Using SystemAddress
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE, 0, systemAddress, false);

   // Using GUID (AddressOrGUID accepts both)
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE, 0, guid, false);

Getting Your Own Address
------------------------

.. code-block:: cpp

   // Get your external address (as seen by a specific peer)
   MafiaNet::SystemAddress myAddr = peer->GetExternalID(remoteAddr);

   // Get your local/internal address
   MafiaNet::SystemAddress localAddr = peer->GetInternalID();

IPv6
----

For IPv6 addresses:

.. code-block:: cpp

   MafiaNet::SystemAddress addr6;
   addr6.FromString("fe80::7c:31f7:fec4:27de%14");
   addr6.SetPortHostOrder(60000);

   // Check if IPv6
   if (addr6.IsIPV6()) {
       // Handle IPv6
   }

See Also
--------

* :doc:`connecting` - Using addresses for connections
* :doc:`../guide/concepts` - Core networking concepts
