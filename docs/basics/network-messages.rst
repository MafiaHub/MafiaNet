Network Messages
================

MafiaNet uses message IDs to identify packet types. The first byte of every packet is the message ID.

Built-in Message IDs
--------------------

MafiaNet reserves IDs 0-134 for internal use. These are defined in ``MessageIdentifiers.h``.

Connection Messages
~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - ID
     - Description
   * - ``ID_CONNECTION_REQUEST_ACCEPTED``
     - Connection accepted by remote system
   * - ``ID_CONNECTION_ATTEMPT_FAILED``
     - Connection attempt failed
   * - ``ID_NEW_INCOMING_CONNECTION``
     - New client connected (server-side)
   * - ``ID_NO_FREE_INCOMING_CONNECTIONS``
     - Server has no free slots
   * - ``ID_DISCONNECTION_NOTIFICATION``
     - Remote system disconnected cleanly
   * - ``ID_CONNECTION_LOST``
     - Connection timed out
   * - ``ID_CONNECTION_BANNED``
     - Banned from connecting
   * - ``ID_INVALID_PASSWORD``
     - Wrong password
   * - ``ID_ALREADY_CONNECTED``
     - Already connected to this system

Ping Messages
~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - ID
     - Description
   * - ``ID_UNCONNECTED_PING``
     - Offline ping request
   * - ``ID_UNCONNECTED_PONG``
     - Offline ping response
   * - ``ID_CONNECTED_PING``
     - Connected ping
   * - ``ID_CONNECTED_PONG``
     - Connected pong

Timestamp
~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - ID
     - Description
   * - ``ID_TIMESTAMP``
     - Precedes timestamped messages

Plugin Messages
~~~~~~~~~~~~~~~

Many IDs are used by plugins (NAT punchthrough, file transfer, etc.). See the respective plugin documentation.

Custom Message IDs
------------------

Define your custom messages starting at ``ID_USER_PACKET_ENUM``:

.. code-block:: cpp

   // GameMessages.h
   enum GameMessages {
       // Start at ID_USER_PACKET_ENUM (guaranteed to be after built-in IDs)
       ID_GAME_STATE_UPDATE = ID_USER_PACKET_ENUM,
       ID_PLAYER_INPUT,
       ID_PLAYER_POSITION,
       ID_PLAYER_SHOOT,
       ID_PLAYER_HIT,
       ID_PLAYER_DIED,
       ID_CHAT_MESSAGE,
       ID_SERVER_MESSAGE,
       // Add more as needed
   };

Organizing Messages
~~~~~~~~~~~~~~~~~~~

For larger projects, organize by category:

.. code-block:: cpp

   enum GameMessages {
       // Player messages (100-199)
       ID_PLAYER_FIRST = ID_USER_PACKET_ENUM + 100,
       ID_PLAYER_SPAWN,
       ID_PLAYER_POSITION,
       ID_PLAYER_INPUT,
       ID_PLAYER_DIED,

       // World messages (200-299)
       ID_WORLD_FIRST = ID_USER_PACKET_ENUM + 200,
       ID_WORLD_STATE,
       ID_OBJECT_SPAWN,
       ID_OBJECT_DESTROY,

       // Chat messages (300-399)
       ID_CHAT_FIRST = ID_USER_PACKET_ENUM + 300,
       ID_CHAT_MESSAGE,
       ID_CHAT_PRIVATE,
   };

Message Handling Pattern
------------------------

.. code-block:: cpp

   void HandlePacket(MafiaNet::Packet* packet) {
       unsigned char msgId = packet->data[0];

       // Handle timestamp prefix
       MafiaNet::BitStream bs(packet->data, packet->length, false);
       MafiaNet::Time timestamp = 0;

       if (msgId == ID_TIMESTAMP) {
           bs.IgnoreBytes(1);
           bs.Read(timestamp);
           bs.Read(msgId);
       } else {
           bs.IgnoreBytes(1);
       }

       // Route to appropriate handler
       if (msgId < ID_USER_PACKET_ENUM) {
           HandleSystemMessage(msgId, packet, timestamp);
       } else {
           HandleGameMessage(msgId, bs, packet, timestamp);
       }
   }

   void HandleSystemMessage(unsigned char msgId, MafiaNet::Packet* packet,
                            MafiaNet::Time timestamp) {
       switch (msgId) {
           case ID_CONNECTION_REQUEST_ACCEPTED:
               OnConnected();
               break;
           case ID_DISCONNECTION_NOTIFICATION:
           case ID_CONNECTION_LOST:
               OnDisconnected(packet->guid);
               break;
           // ... other system messages
       }
   }

   void HandleGameMessage(unsigned char msgId, MafiaNet::BitStream& bs,
                          MafiaNet::Packet* packet, MafiaNet::Time timestamp) {
       switch (msgId) {
           case ID_PLAYER_POSITION:
               HandlePlayerPosition(bs, packet->guid, timestamp);
               break;
           case ID_CHAT_MESSAGE:
               HandleChatMessage(bs, packet->guid);
               break;
           // ... other game messages
       }
   }

See Also
--------

* :doc:`creating-packets` - Creating custom packets
* :doc:`receiving-packets` - Receiving and parsing packets
* :doc:`timestamping` - Using timestamps
