Core Concepts
=============

This guide covers the fundamental concepts of MafiaNet.

Peers and Connections
---------------------

In MafiaNet, every participant in the network is a **peer**. The main interface for network communication is ``RakPeerInterface``.

.. code-block:: cpp

   // Create a peer
   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   // Clean up when done
   MafiaNet::RakPeerInterface::DestroyInstance(peer);

System Addresses
----------------

Every peer has a unique ``SystemAddress`` that identifies it on the network. This includes the IP address and port.

.. code-block:: cpp

   MafiaNet::SystemAddress address;
   address.SetBinaryAddress("192.168.1.100");
   address.SetPortHostOrder(60000);

   // Or from string
   MafiaNet::SystemAddress addr("192.168.1.100:60000");

Packets
-------

Network data is received as ``Packet`` objects. Each packet contains:

* ``data`` - Raw byte array of the message
* ``length`` - Size of the data
* ``systemAddress`` - Address of the sender
* ``guid`` - Unique identifier of the sender

.. code-block:: cpp

   MafiaNet::Packet* packet = peer->Receive();
   if (packet) {
       // First byte is always the message type
       unsigned char messageType = packet->data[0];

       // Process based on type
       switch (messageType) {
           case ID_NEW_INCOMING_CONNECTION:
               // Handle connection
               break;
       }

       // Always deallocate when done
       peer->DeallocatePacket(packet);
   }

Message Priorities
------------------

Messages can be sent with different priorities:

* ``IMMEDIATE_PRIORITY`` - Sent immediately, bypasses normal send queue
* ``HIGH_PRIORITY`` - Sent before medium and low priority messages
* ``MEDIUM_PRIORITY`` - Default priority level
* ``LOW_PRIORITY`` - Sent after higher priority messages

Reliability Types
-----------------

MafiaNet supports several reliability modes:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Type
     - Description
   * - ``UNRELIABLE``
     - May be lost, may arrive out of order. Fastest.
   * - ``UNRELIABLE_SEQUENCED``
     - May be lost, but if received will be in order. Old packets are dropped.
   * - ``RELIABLE``
     - Will arrive, but may be out of order.
   * - ``RELIABLE_ORDERED``
     - Will arrive in order. Packets wait for missing packets.
   * - ``RELIABLE_SEQUENCED``
     - Will arrive in order. Old packets are dropped.

Ordering Channels
-----------------

Messages can be sent on different ordering channels (0-31). Messages on different channels are ordered independently.

.. code-block:: cpp

   // Channel 0 for game state, channel 1 for chat
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, address, false);  // Game state
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 1, address, false);  // Chat

BitStream
---------

``BitStream`` is the primary way to serialize and deserialize data:

.. code-block:: cpp

   // Writing
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_GAME_MESSAGE);
   bs.Write(playerPosition.x);
   bs.Write(playerPosition.y);
   bs.WriteCompressed(health);  // Uses less bandwidth for small values

   // Reading
   MafiaNet::BitStream bsIn(packet->data, packet->length, false);
   bsIn.IgnoreBytes(1);  // Skip message ID
   float x, y;
   bsIn.Read(x);
   bsIn.Read(y);
   int health;
   bsIn.ReadCompressed(health);
