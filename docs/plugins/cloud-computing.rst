CloudComputing Plugin
=====================

The CloudComputing plugins (CloudClient and CloudServer) provide distributed memory
and service discovery. Servers can upload key-value data that clients can query
and subscribe to across multiple cloud servers.

Basic Usage
-----------

**Cloud server setup:**

.. code-block:: cpp

   #include "slikenet/CloudServer.h"

   MafiaNet::CloudServer cloudServer;
   peer->AttachPlugin(&cloudServer);

   // Configure server capacity
   cloudServer.SetMaxUploadBytesPerClient(1024 * 1024);  // 1MB per client
   cloudServer.SetMaxBytesPerUpload(64 * 1024);  // 64KB per upload

**Cloud client posting data:**

.. code-block:: cpp

   #include "slikenet/CloudClient.h"

   MafiaNet::CloudClient cloudClient;
   peer->AttachPlugin(&cloudClient);

   // Post data to the cloud
   MafiaNet::CloudKey key;
   key.primaryKey = "GameServer";
   key.secondaryKey = 1;  // Server ID

   MafiaNet::BitStream data;
   data.Write("My Game Server");
   data.Write(playerCount);
   data.Write(maxPlayers);

   cloudClient.Post(&key, data.GetData(), data.GetNumberOfBytesUsed(),
                    cloudServerAddress);

**Querying cloud data:**

.. code-block:: cpp

   // Query for all game servers
   MafiaNet::CloudQuery query;
   query.keys.Push(MafiaNet::CloudKey("GameServer", 0),
                   _FILE_AND_LINE_);
   query.subscribeToResults = true;  // Get updates

   cloudClient.Get(&query, cloudServerAddress);

**Handling query results:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       if (packet->data[0] == ID_CLOUD_GET_RESPONSE) {
           MafiaNet::CloudQueryResult result;
           cloudClient.OnGetResponse(&result, packet);

           for (unsigned int i = 0; i < result.rowsReturned.Size(); i++) {
               MafiaNet::CloudQueryRow* row = result.rowsReturned[i];
               printf("Server: %s\n", row->clientGUID.ToString());

               MafiaNet::BitStream bs(row->data, row->length, false);
               // Read server info from bitstream
           }
           result.SerializeResult(false, &result);  // Free memory
       }
       peer->DeallocatePacket(packet);
   }

Key Features
------------

* Distributed key-value storage
* Subscription-based updates
* Multi-server federation
* Automatic data expiration
* Service discovery pattern
* Server browser implementation

Configuration Options
---------------------

* ``Post()`` - Upload data to cloud
* ``Get()`` - Query cloud data
* ``Unsubscribe()`` - Stop receiving updates
* ``SetMaxUploadBytesPerClient()`` - Per-client limits
* ``SetMaxBytesPerUpload()`` - Per-upload limits

See Also
--------

* :doc:`lobby2` - Higher-level matchmaking
* :doc:`../basics/bitstreams` - Data serialization
* :doc:`fully-connected-mesh-2` - P2P discovery
