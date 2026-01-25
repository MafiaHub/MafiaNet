NetworkIDObject
===============

``NetworkIDObject`` provides a way to assign unique identifiers to objects that all systems can reference.

Purpose
-------

In multiplayer games, you need a way to refer to the same object across different machines. ``NetworkIDObject`` solves this by:

- Assigning unique IDs to objects
- Allowing lookup of objects by ID
- Managing object ID allocation

Basic Usage
-----------

Inherit from NetworkIDObject
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   #include "mafianet/NetworkIDObject.h"

   class Player : public MafiaNet::NetworkIDObject {
   public:
       float x, y, z;
       int health;
       std::string name;
   };

Creating and Registering Objects
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The authority (usually server) creates objects and assigns IDs:

.. code-block:: cpp

   MafiaNet::NetworkIDManager networkIdManager;

   // Create a player
   Player* player = new Player();
   player->SetNetworkIDManager(&networkIdManager);

   // ID is automatically assigned
   MafiaNet::NetworkID id = player->GetNetworkID();

   // Send the ID to clients
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_SPAWN_PLAYER);
   bs.Write(id);
   bs.Write(player->x);
   bs.Write(player->y);
   bs.Write(player->z);
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, addr, true);

Receiving and Looking Up Objects
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   case ID_SPAWN_PLAYER: {
       MafiaNet::NetworkID id;
       float x, y, z;
       bs.Read(id);
       bs.Read(x);
       bs.Read(y);
       bs.Read(z);

       // Create local representation
       Player* player = new Player();
       player->SetNetworkIDManager(&networkIdManager);
       player->SetNetworkID(id);  // Use the same ID as server
       player->x = x;
       player->y = y;
       player->z = z;
       break;
   }

   case ID_PLAYER_POSITION: {
       MafiaNet::NetworkID id;
       float x, y, z;
       bs.Read(id);
       bs.Read(x);
       bs.Read(y);
       bs.Read(z);

       // Look up the player
       Player* player = (Player*)networkIdManager.GET_OBJECT_FROM_ID<Player*>(id);
       if (player) {
           player->x = x;
           player->y = y;
           player->z = z;
       }
       break;
   }

NetworkIDManager
----------------

The ``NetworkIDManager`` maintains the mapping between IDs and objects.

.. code-block:: cpp

   MafiaNet::NetworkIDManager networkIdManager;

   // Set whether this instance is the authority (assigns IDs)
   networkIdManager.SetIsNetworkIDAuthority(isServer);

   // Look up object by ID
   void* obj = networkIdManager.GET_BASE_OBJECT_FROM_ID(id);

   // Typed lookup
   Player* player = networkIdManager.GET_OBJECT_FROM_ID<Player*>(id);

Authority vs Client
-------------------

**Authority (Server)**:

- Creates NetworkIDs
- ``SetIsNetworkIDAuthority(true)``
- IDs are automatically assigned

**Client**:

- Receives NetworkIDs from server
- ``SetIsNetworkIDAuthority(false)``
- Uses ``SetNetworkID()`` to assign received IDs

Object Deletion
---------------

When destroying networked objects:

.. code-block:: cpp

   // On authority (server)
   void DestroyPlayer(Player* player) {
       MafiaNet::NetworkID id = player->GetNetworkID();

       // Notify clients
       MafiaNet::BitStream bs;
       bs.Write((MafiaNet::MessageID)ID_DESTROY_PLAYER);
       bs.Write(id);
       peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, addr, true);

       delete player;  // Automatically unregisters from manager
   }

   // On client
   case ID_DESTROY_PLAYER: {
       MafiaNet::NetworkID id;
       bs.Read(id);

       Player* player = networkIdManager.GET_OBJECT_FROM_ID<Player*>(id);
       if (player) {
           delete player;
       }
       break;
   }

Integration with ReplicaManager3
--------------------------------

For more advanced object replication, consider using :doc:`../plugins/replica-manager-3`, which builds on NetworkIDObject and provides:

- Automatic object creation/destruction across network
- Serialization management
- Scoping (which clients see which objects)
- Reference pointer serialization

See Also
--------

* :doc:`../plugins/replica-manager-3` - Advanced object replication
* :doc:`creating-packets` - Sending object updates
