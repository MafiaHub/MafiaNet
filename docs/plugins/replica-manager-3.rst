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

   #include "mafianet/replica_manager3.h"

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
       std::string name;

       // Allocation ID for object creation
       virtual void WriteAllocationID(
           MafiaNet::Connection_RM3* conn,
           MafiaNet::BitStream* bs) const override {
           bs->Write(std::string("Player"));
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

           std::string typeName;
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

Static objects
--------------

For an object that every peer creates locally, such as a door loaded with a
level, assign the same unique network ID on every peer. Call
``SetNetworkIDManager()`` on the replica with the manager used by
``ReplicaManager3``, then call ``SetNetworkID()`` before ``Reference()``.

Have the authoritative peer return ``RM3CS_ALREADY_EXISTS_REMOTELY`` from
``QueryConstruction()``. It can send initial state through
``SerializeConstructionExisting()``, which the receiver reads in
``DeserializeConstructionExisting()`` on its local object. Other peers return
``RM3CS_ALREADY_EXISTS_REMOTELY_DO_NOT_CONSTRUCT``. Send subsequent state
changes through ``Serialize()`` on the authoritative peer. For a level-owned
object that survives a disconnect, return ``RM3AOPC_DO_NOTHING`` from
``QueryActionOnPopConnection()``.

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

Initial state and late joiners
------------------------------

A newly constructed replica can produce three copies of the same field in its
first server tick. ``SerializeConstruction()`` writes the construction record,
the initial download calls ``Serialize()`` for the new connection, and the
ordinary serialization pass checks for changes. The callbacks can describe
different data, so the manager treats each result independently.

For a replica that sends identical state to all observers, one option is to put
a complete snapshot in ``SerializeConstruction()`` and apply it in
``DeserializeConstruction()``. Opt into ``BroadcastsIdentically()`` and mark the
replica dirty when its shared value changes. Initialize the flag to false. Its
construction-time, directed ``Serialize()`` call can return
``RM3SR_DO_NOT_SERIALIZE``. The shared broadcast call returns that result while clean. When dirty, it writes the value
once, clears the flag, and returns
``RM3SR_BROADCAST_IDENTICALLY``. A joining client receives the current snapshot.
Reading that snapshot must leave any pending broadcast available to existing
observers.

The manual ``ReplicaManager3_Construct1000`` benchmark exercises this pattern
with a single four-byte field and reports bytes for construction records, download
markers, initial state messages, and ordinary state messages.

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

For runtime **dimension** scoping — separating players so they only see others in
the same virtual world (e.g. apartments) — see :doc:`virtual-worlds`, which
provides this on top of ReplicaManager3 without per-object filtering code.

Ordering Channels
-----------------

Construction, destruction and scope-change messages go out ``ReliableOrdered`` on the channel set with ``SetDefaultOrderingChannel()`` (default 0). Serialize channels are sent with the priority, reliability and ordering channel in ``SerializeParameters::pro[channel]``, which default to the manager's settings; channels with the same settings share one message, and a channel with nothing selected to send costs no message. Set them inside ``Serialize()``:

.. code-block:: cpp

   virtual MafiaNet::RM3SerializationResult Serialize(MafiaNet::SerializeParameters* params) override {
       // Pose: unreliable, alone on its channel, so a lost reliable message never delays it.
       params->outputBitstream[0].Write(position);
       params->pro[0].reliability     = MafiaNet::Reliability::Unreliable;
       params->pro[0].orderingChannel = 0;

       // State that must arrive, reliable-ordered on another channel.
       params->outputBitstream[1].Write(health);
       params->pro[1].reliability     = MafiaNet::Reliability::ReliableOrdered;
       params->pro[1].orderingChannel = 1;
       return MafiaNet::RM3SR_BROADCAST_IDENTICALLY;
   }

Send the reliable RPCs that name replicas on the manager's default channel, so a message about an object cannot overtake its construction. Sequenced serialization shares one sequence stream per channel across every replica, so a reordered update for one replica discards every other replica's older update; with many replicas prefer ``Unreliable`` plus ``timeStamp`` ordering in ``Deserialize()``. See :ref:`ordering-channels`.

See Also
--------

* :doc:`overview` - Plugin basics
* :doc:`virtual-worlds` - Per-player dimension/visibility scoping
* :doc:`../basics/network-id-object` - NetworkIDObject
