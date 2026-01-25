ReadyEvent Plugin
=================

The ReadyEvent plugin synchronizes ready states across multiple systems. It's useful
for implementing game lobbies, synchronized starts, and coordinating multi-player
events where all participants must signal readiness.

Basic Usage
-----------

**Setting up ready events:**

.. code-block:: cpp

   #include "slikenet/ReadyEvent.h"

   MafiaNet::ReadyEvent readyEvent;
   peer->AttachPlugin(&readyEvent);

   // Define event IDs
   enum GameEvents {
       EVENT_GAME_START = 0,
       EVENT_LEVEL_LOADED = 1,
       EVENT_ROUND_READY = 2
   };

   // Add systems that must be ready
   readyEvent.AddToWaitList(EVENT_GAME_START, player1Guid);
   readyEvent.AddToWaitList(EVENT_GAME_START, player2Guid);
   readyEvent.AddToWaitList(EVENT_GAME_START, player3Guid);

**Signaling readiness:**

.. code-block:: cpp

   // Local player is ready
   readyEvent.SetEvent(EVENT_GAME_START, true);

   // Check if all are ready
   if (readyEvent.IsEventSet(EVENT_GAME_START)) {
       printf("All players ready - starting game!\n");
   }

**Handling ready events:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       switch (packet->data[0]) {
           case ID_READY_EVENT_SET:
               printf("Player ready: %s\n",
                      packet->guid.ToString());
               break;

           case ID_READY_EVENT_UNSET:
               printf("Player not ready: %s\n",
                      packet->guid.ToString());
               break;

           case ID_READY_EVENT_ALL_SET:
               printf("All players ready for event!\n");
               StartGame();
               break;
       }
       peer->DeallocatePacket(packet);
   }

Key Features
------------

* Multiple concurrent ready events with unique IDs
* Dynamic wait list management (add/remove participants)
* Automatic notification when all systems are ready
* Support for ready state toggling
* Query individual or group ready status
* Event completion callbacks

Configuration Options
---------------------

* ``AddToWaitList()`` - Add system to event wait list
* ``RemoveFromWaitList()`` - Remove system from wait list
* ``SetEvent()`` - Set local ready state
* ``IsEventSet()`` - Check if event is ready
* ``IsEventCompletionProcessing()`` - Check if completion in progress
* ``GetEventListSize()`` - Number of systems in wait list

See Also
--------

* :doc:`fully-connected-mesh-2` - P2P coordination
* :doc:`team-manager` - Team-based ready states
* :doc:`lobby2` - Lobby system integration
