Receiving Packets
=================

Receive packets by polling ``Receive()`` in your game loop.

The Receive Loop
----------------

.. code-block:: cpp

   MafiaNet::Packet* packet;
   for (packet = peer->Receive(); packet;
        peer->DeallocatePacket(packet), packet = peer->Receive()) {

       // Process packet
       ProcessPacket(packet);
   }

.. warning::
   Always call ``DeallocatePacket()`` after processing. Failure to do so causes memory leaks.

The Packet Structure
--------------------

.. code-block:: cpp

   struct Packet {
       SystemAddress systemAddress;  // Sender's address
       RakNetGUID guid;              // Sender's unique ID
       unsigned int length;          // Data length in bytes
       BitSize_t bitSize;            // Data length in bits
       unsigned char* data;          // The actual data
   };

- ``systemAddress`` - IP:port of the sender
- ``guid`` - Unique identifier of the sender (survives reconnects)
- ``data[0]`` - Always the message ID

Processing Messages
-------------------

.. code-block:: cpp

   void ProcessPacket(MafiaNet::Packet* packet) {
       switch (packet->data[0]) {
           // System messages
           case ID_CONNECTION_REQUEST_ACCEPTED:
               OnConnected(packet);
               break;
           case ID_NEW_INCOMING_CONNECTION:
               OnClientConnected(packet);
               break;
           case ID_DISCONNECTION_NOTIFICATION:
           case ID_CONNECTION_LOST:
               OnDisconnected(packet);
               break;

           // Custom messages
           default:
               if (packet->data[0] >= ID_USER_PACKET_ENUM) {
                   OnGameMessage(packet);
               }
               break;
       }
   }

Reading Packet Data
-------------------

Using BitStream
~~~~~~~~~~~~~~~

.. code-block:: cpp

   void OnGameMessage(MafiaNet::Packet* packet) {
       MafiaNet::BitStream bs(packet->data, packet->length, false);

       MafiaNet::MessageID msgId;
       bs.Read(msgId);

       switch (msgId) {
           case ID_PLAYER_POSITION: {
               float x, y, z;
               bs.Read(x);
               bs.Read(y);
               bs.Read(z);
               UpdatePlayerPosition(packet->guid, x, y, z);
               break;
           }
           case ID_CHAT_MESSAGE: {
               MafiaNet::RakString message;
               bs.Read(message);
               DisplayChatMessage(packet->guid, message);
               break;
           }
       }
   }

Using Struct Cast
~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   #pragma pack(push, 1)
   struct PlayerPositionPacket {
       unsigned char msgId;
       float x, y, z;
   };
   #pragma pack(pop)

   case ID_PLAYER_POSITION: {
       PlayerPositionPacket* pos = (PlayerPositionPacket*)packet->data;
       UpdatePlayerPosition(packet->guid, pos->x, pos->y, pos->z);
       break;
   }

Handling Timestamps
-------------------

If a packet starts with ``ID_TIMESTAMP``:

.. code-block:: cpp

   MafiaNet::BitStream bs(packet->data, packet->length, false);

   MafiaNet::MessageID firstByte;
   bs.Read(firstByte);

   MafiaNet::Time timestamp = 0;
   if (firstByte == ID_TIMESTAMP) {
       bs.Read(timestamp);
       bs.Read(firstByte);  // Now read actual message ID
   }

   // timestamp is in your local time (auto-converted by MafiaNet)
   MafiaNet::Time packetAge = MafiaNet::GetTime() - timestamp;

System Message Reference
------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Message ID
     - When Received
   * - ``ID_CONNECTION_REQUEST_ACCEPTED``
     - Connected to server (client)
   * - ``ID_NEW_INCOMING_CONNECTION``
     - Client connected (server)
   * - ``ID_DISCONNECTION_NOTIFICATION``
     - Clean disconnect
   * - ``ID_CONNECTION_LOST``
     - Connection timed out
   * - ``ID_CONNECTION_ATTEMPT_FAILED``
     - Could not connect
   * - ``ID_NO_FREE_INCOMING_CONNECTIONS``
     - Server is full
   * - ``ID_INVALID_PASSWORD``
     - Wrong password
   * - ``ID_ALREADY_CONNECTED``
     - Already connected to this peer
   * - ``ID_UNCONNECTED_PING``
     - Offline ping received
   * - ``ID_UNCONNECTED_PONG``
     - Offline ping response

Validation
----------

Always validate packet data:

.. code-block:: cpp

   // Check minimum length
   if (packet->length < sizeof(PlayerPositionPacket)) {
       printf("Malformed packet from %s\n",
              packet->systemAddress.ToString());
       return;
   }

   // Check read success
   if (!bs.Read(x) || !bs.Read(y) || !bs.Read(z)) {
       printf("Failed to read position data\n");
       return;
   }

   // Validate values
   if (!IsValidPosition(x, y, z)) {
       printf("Invalid position from %s\n",
              packet->systemAddress.ToString());
       return;
   }

Thread Safety
-------------

``Receive()`` is thread-safe. You can call it from any thread, but typically it's called from your main game loop.

For background processing:

.. code-block:: cpp

   // In a network thread
   while (running) {
       MafiaNet::Packet* packet = peer->Receive();
       if (packet) {
           // Queue for main thread processing
           packetQueue.Push(packet);
       } else {
           Sleep(1);
       }
   }

   // In main thread
   MafiaNet::Packet* packet;
   while (packetQueue.Pop(packet)) {
       ProcessPacket(packet);
       peer->DeallocatePacket(packet);
   }

See Also
--------

* :doc:`creating-packets` - Creating packets
* :doc:`network-messages` - Message ID reference
* :doc:`bitstreams` - BitStream details
