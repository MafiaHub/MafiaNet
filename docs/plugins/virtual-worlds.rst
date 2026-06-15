Virtual Worlds (Dimensions)
===========================

Virtual worlds let you separate players into **dimensions** at runtime: a player
in one virtual world only sees the players, vehicles, and objects that share
that virtual world. This is the SA-MP ``SetPlayerVirtualWorld`` /
FiveM routing-bucket model — ideal for instanced interiors such as apartments,
minigame arenas, or per-player cutscenes. Switching dimension is a single value
change with no reconnect.

The feature is layered on :doc:`replica-manager-3` and only scopes
RM3-replicated entities.

Virtual world vs. RM3 ``WorldId``
---------------------------------

These are two different concepts — they compose:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Concept
     - Meaning
   * - RM3 ``WorldId`` (``AddWorld``)
     - Heavyweight, separate **instance**: its own ``NetworkIDManager``,
       connection list, and replica list. Few of them, set up ahead of time.
   * - ``VirtualWorldId``
     - Lightweight per-entity / per-observer **tag**. Everyone stays on the same
       connection and the same RM3 world; visibility is filtered by the tag.

Use a virtual world for fine-grained, frequently-switched visibility scoping
(apartments, dimensions). Use an RM3 ``WorldId`` only when you truly need an
isolated instance.

How visibility works
--------------------

Each entity and each observer carries one ``VirtualWorldId``. Two subjects can
see each other when their ids are equal, or when either side is the reserved
global value:

.. code-block:: cpp

   #include "mafianet/VirtualWorld.h"

   typedef uint32_t VirtualWorldId;                  // many dimensions
   static const VirtualWorldId VIRTUAL_WORLD_DEFAULT = 0;          // the overworld
   static const VirtualWorldId VIRTUAL_WORLD_GLOBAL  = 0xFFFFFFFF; // visible everywhere

   bool VirtualWorldsCanSee(VirtualWorldId a, VirtualWorldId b); // a==b || either is GLOBAL

``VIRTUAL_WORLD_GLOBAL`` is handy for shared world geometry, global NPCs, or
admins/spectators that should be visible across all dimensions.

Making entities virtual-world aware
-----------------------------------

Derive your replicated objects from ``VirtualWorldReplica3`` instead of
``Replica3``. The base class applies the virtual world filter in
``QueryConstruction`` / ``QueryDestruction`` / ``QuerySerialization`` and then
delegates to ``*WithinWorld()`` hooks, which you implement with the usual
topology defaults. You do **not** override the ``Query*`` methods themselves.

.. code-block:: cpp

   #include "mafianet/VirtualWorldReplica3.h"

   class Player : public MafiaNet::VirtualWorldReplica3 {
   public:
       // ... your usual Replica3 overrides (WriteAllocationID, Serialize,
       //     Deserialize, SerializeConstruction, DeserializeConstruction,
       //     SerializeDestruction, DeserializeDestruction, DeallocReplica,
       //     QueryRemoteConstruction, QueryActionOnPopConnection) ...

   protected:
       // Reached only when the observer shares this entity's virtual world.
       // Return the topology default for your architecture.
       MafiaNet::RM3ConstructionState QueryConstructionWithinWorld(
           MafiaNet::Connection_RM3* dest, MafiaNet::ReplicaManager3* rm3) override {
           return QueryConstruction_ServerConstruction(dest, /*isThisTheServer*/ true);
       }
       MafiaNet::RM3QuerySerializationResult QuerySerializationWithinWorld(
           MafiaNet::Connection_RM3* dest) override {
           return QuerySerialization_ServerSerializable(dest, /*isThisTheServer*/ true);
       }
       // QueryDestructionWithinWorld() defaults to RM3DS_NO_ACTION; override only
       // if you also drive per-connection destruction from within a world.
   };

Set an entity's virtual world with ``SetVirtualWorld()`` / read it with
``GetVirtualWorld()``. New entities start in ``VIRTUAL_WORLD_DEFAULT``.

Setting an observer's virtual world
-----------------------------------

Each connection (observer) has its own virtual world — the dimension that player
currently perceives:

.. code-block:: cpp

   MafiaNet::Connection_RM3* conn = rm3->GetConnectionByGUID(playerGuid);
   conn->SetVirtualWorld(apartmentId);
   VirtualWorldId vw = conn->GetVirtualWorld(); // defaults to VIRTUAL_WORLD_DEFAULT

Moving a player to a dimension
------------------------------

Moving a player usually means two things at once: the dimension they *perceive*
(their connection) and how others *see them* (their avatar entity).
``SetPlayerVirtualWorld()`` does both:

.. code-block:: cpp

   // Drop a player into apartment 7. The engine spawns the apartment's contents
   // in and despawns the overworld on the next ReplicaManager3::Update(), and
   // vice-versa when they leave -- no reconnect, no Pop/Push of connections.
   rm3->SetPlayerVirtualWorld(playerConn, playerAvatar, 7);

   // Send them back to the overworld:
   rm3->SetPlayerVirtualWorld(playerConn, playerAvatar, MafiaNet::VIRTUAL_WORLD_DEFAULT);

Runtime switching works automatically in the default construction mode
(``QUERY_REPLICA_FOR_CONSTRUCTION_AND_DESTRUCTION``), which re-evaluates
construction and destruction every tick.

Scoping non-replica traffic (chat, RPC, raw sends)
--------------------------------------------------

Virtual worlds only filter RM3-replicated entities. To scope other traffic to a
dimension yourself, query who is in it:

.. code-block:: cpp

   DataStructures::List<MafiaNet::RakNetGUID> recipients;
   rm3->GetGuidsInVirtualWorld(apartmentId, recipients); // includeGlobal=true by default
   for (unsigned i = 0; i < recipients.Size(); i++)
       peer->Send(&chatMsg, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, recipients[i], false);

   // Or get the connection objects directly:
   DataStructures::List<MafiaNet::Connection_RM3*> conns;
   rm3->GetConnectionsInVirtualWorld(apartmentId, conns, /*includeGlobal*/ true);

Pass ``includeGlobal=false`` to exclude observers in ``VIRTUAL_WORLD_GLOBAL``.

.. important::

   **The virtual world filter is applied only by the authority** for a given
   (entity, connection) pair — the system that would actually construct the
   entity toward that connection. A downloaded copy on a non-authority (for
   example a server-owned entity replicated to a client) defers to the topology
   default and never suppresses or destroys the entity upstream. This is handled
   for you by ``VirtualWorldReplica3``; keep it in mind if you write custom
   visibility logic of your own.

Example: an apartment
---------------------

.. code-block:: cpp

   // Server-authoritative: the server owns each player's avatar and decides its
   // dimension. Both avatars start in the overworld and see each other.
   serverRm->Reference(avatarA); // virtual world 0
   serverRm->Reference(avatarB); // virtual world 0

   // Player B enters apartment 1 -> A and B can no longer see each other.
   serverRm->SetPlayerVirtualWorld(connToB, avatarB, 1);

   // Player B leaves -> they see each other again.
   serverRm->SetPlayerVirtualWorld(connToB, avatarB, MafiaNet::VIRTUAL_WORLD_DEFAULT);

A complete, runnable demonstration (server + two clients) lives in
``Samples/VirtualWorld``.

See Also
--------

* :doc:`replica-manager-3` - The replication system this builds on
* :doc:`../basics/network-id-object` - NetworkIDObject
