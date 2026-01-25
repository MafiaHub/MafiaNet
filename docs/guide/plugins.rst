Plugins
=======

MafiaNet's plugin system allows extending functionality without modifying core code.

Using Plugins
-------------

Plugins are attached to a ``RakPeerInterface``:

.. code-block:: cpp

   MafiaNet::SomePlugin* plugin = MafiaNet::SomePlugin::GetInstance();
   peer->AttachPlugin(plugin);

   // Later, to remove
   peer->DetachPlugin(plugin);
   MafiaNet::SomePlugin::DestroyInstance(plugin);

Available Plugins
-----------------

**Networking Plugins**

* ``FullyConnectedMesh2`` - Maintains fully connected peer networks
* ``NatPunchthroughClient/Server`` - NAT traversal support
* ``Router2`` - Route packets through intermediate peers
* ``UDPProxyClient/Server/Coordinator`` - UDP relay when NAT fails

**Data Transfer Plugins**

* ``FileListTransfer`` - Transfer files with compression and delta updates
* ``DirectoryDeltaTransfer`` - Sync directory contents between peers

**Game Plugins**

* ``ReplicaManager3`` - Object replication and state synchronization
* ``ReadyEvent`` - Synchronize ready state across peers
* ``TeamManager`` - Manage teams and team balancing
* ``TeamBalancer`` - Automatic team balancing

**Utility Plugins**

* ``RPC4Plugin`` - Remote procedure calls
* ``MessageFilter`` - Filter messages by type
* ``PacketLogger`` - Log all network traffic
* ``TwoWayAuthentication`` - Secure authentication

ReplicaManager3
---------------

The ``ReplicaManager3`` plugin handles object replication:

.. code-block:: cpp

   #include "mafianet/ReplicaManager3.h"

   class MyReplica : public MafiaNet::Replica3 {
   public:
       virtual void WriteAllocationID(MafiaNet::Connection_RM3* conn,
                                      MafiaNet::BitStream* bs) const override {
           bs->Write(MafiaNet::RakString("MyReplica"));
       }

       virtual MafiaNet::RM3SerializationResult Serialize(
           MafiaNet::SerializeParameters* params) override {
           params->outputBitstream[0].Write(position);
           params->outputBitstream[0].Write(rotation);
           return MafiaNet::RM3SR_BROADCAST_IDENTICALLY;
       }

       virtual void Deserialize(MafiaNet::DeserializeParameters* params) override {
           params->serializationBitstream[0].Read(position);
           params->serializationBitstream[0].Read(rotation);
       }
   };

RPC4Plugin
----------

Call functions on remote peers:

.. code-block:: cpp

   #include "mafianet/RPC4Plugin.h"

   MafiaNet::RPC4* rpc = MafiaNet::RPC4::GetInstance();
   peer->AttachPlugin(rpc);

   // Register a function
   rpc->RegisterSlot("SpawnPlayer", SpawnPlayerCallback, 0);

   // Call remotely
   MafiaNet::BitStream bs;
   bs.Write(playerId);
   bs.Write(spawnPosition);
   rpc->Signal("SpawnPlayer", &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
               targetAddress, false, false);

Creating Custom Plugins
-----------------------

Extend ``PluginInterface2``:

.. code-block:: cpp

   class MyPlugin : public MafiaNet::PluginInterface2 {
   public:
       virtual MafiaNet::PluginReceiveResult OnReceive(
           MafiaNet::Packet* packet) override {
           // Process packets before they reach the application
           if (packet->data[0] == MY_CUSTOM_MESSAGE) {
               HandleCustomMessage(packet);
               return MafiaNet::RR_STOP_PROCESSING_AND_DEALLOCATE;
           }
           return MafiaNet::RR_CONTINUE_PROCESSING;
       }

       virtual void OnNewConnection(
           const MafiaNet::SystemAddress& address,
           MafiaNet::RakNetGUID guid,
           bool isIncoming) override {
           // Handle new connections
       }

       virtual void OnClosedConnection(
           const MafiaNet::SystemAddress& address,
           MafiaNet::RakNetGUID guid,
           MafiaNet::PI2_LostConnectionReason reason) override {
           // Handle disconnections
       }
   };
