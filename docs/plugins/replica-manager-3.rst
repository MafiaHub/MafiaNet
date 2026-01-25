ReplicaManager3
===============

ReplicaManager3 automates object replication across the network.

Overview
--------

ReplicaManager3 handles:

- Object creation and destruction notification
- Automatic serialization
- Scoping (which clients see which objects)
- Reference pointer serialization

Setup
-----

.. code-block:: cpp

   #include "mafianet/ReplicaManager3.h"

   // Create and attach
   MafiaNet::ReplicaManager3* replicaManager =
       MafiaNet::ReplicaManager3::GetInstance();
   peer->AttachPlugin(replicaManager);

   // Set network ID manager
   replicaManager->SetNetworkIDManager(&networkIdManager);

Creating Replica Objects
------------------------

Inherit from ``Replica3``:

.. code-block:: cpp

   class Player : public MafiaNet::Replica3 {
   public:
       float x, y, z;
       int health;
       MafiaNet::RakString name;

       // Allocation ID for object creation
       virtual void WriteAllocationID(
           MafiaNet::Connection_RM3* conn,
           MafiaNet::BitStream* bs) const override {
           bs->Write(MafiaNet::RakString("Player"));
       }

       // Serialize object state
       virtual MafiaNet::RM3SerializationResult Serialize(
           MafiaNet::SerializeParameters* params) override {

           params->outputBitstream[0].Write(x);
           params->outputBitstream[0].Write(y);
           params->outputBitstream[0].Write(z);
           params->outputBitstream[0].Write(health);
           params->outputBitstream[0].Write(name);

           return MafiaNet::RM3SR_BROADCAST_IDENTICALLY;
       }

       // Deserialize object state
       virtual void Deserialize(
           MafiaNet::DeserializeParameters* params) override {

           params->serializationBitstream[0].Read(x);
           params->serializationBitstream[0].Read(y);
           params->serializationBitstream[0].Read(z);
           params->serializationBitstream[0].Read(health);
           params->serializationBitstream[0].Read(name);
       }

       // When to serialize
       virtual MafiaNet::RM3QuerySerializationResult QuerySerialization(
           MafiaNet::Connection_RM3* conn) override {
           return MafiaNet::RM3QSR_CALL_SERIALIZE;
       }
   };

Connection Class
----------------

Create a connection class for each connected peer:

.. code-block:: cpp

   class GameConnection : public MafiaNet::Connection_RM3 {
   public:
       virtual MafiaNet::Replica3* AllocReplica(
           MafiaNet::BitStream* allocationIdBitstream,
           MafiaNet::ReplicaManager3* rm3) override {

           MafiaNet::RakString typeName;
           allocationIdBitstream->Read(typeName);

           if (typeName == "Player") {
               return new Player();
           }
           return nullptr;
       }
   };

   // In your ReplicaManager3 subclass
   class GameReplicaManager : public MafiaNet::ReplicaManager3 {
   public:
       virtual MafiaNet::Connection_RM3* AllocConnection(
           const MafiaNet::SystemAddress& addr,
           MafiaNet::RakNetGUID guid) const override {
           return new GameConnection();
       }

       virtual void DeallocConnection(
           MafiaNet::Connection_RM3* conn) const override {
           delete conn;
       }
   };

Registering Objects
-------------------

.. code-block:: cpp

   // Server creates and references
   Player* player = new Player();
   player->x = 100;
   player->y = 0;
   player->z = 50;
   replicaManager->Reference(player);

   // Automatically replicated to all connections!

Serialization Results
---------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Result
     - Description
   * - ``RM3SR_BROADCAST_IDENTICALLY``
     - Same data to all
   * - ``RM3SR_BROADCAST_IDENTICALLY_FORCE_SERIALIZATION``
     - Same data, force send
   * - ``RM3SR_SERIALIZED_UNIQUELY``
     - Different data per connection
   * - ``RM3SR_DO_NOT_SERIALIZE``
     - Skip this serialization

Scoping
-------

Control which connections see which objects:

.. code-block:: cpp

   virtual MafiaNet::RM3QuerySerializationResult QuerySerialization(
       MafiaNet::Connection_RM3* conn) override {

       // Only serialize to nearby players
       Player* other = GetPlayerFromConnection(conn);
       if (Distance(this, other) > VIEW_DISTANCE) {
           return MafiaNet::RM3QSR_DO_NOT_CALL_SERIALIZE;
       }
       return MafiaNet::RM3QSR_CALL_SERIALIZE;
   }

See Also
--------

* :doc:`overview` - Plugin basics
* :doc:`../basics/network-id-object` - NetworkIDObject
