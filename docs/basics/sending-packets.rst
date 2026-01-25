Sending Packets
===============

Once you've created a packet, send it using ``Send()`` or ``SendList()``.

The Send Function
-----------------

.. code-block:: cpp

   uint32_t Send(
       const char *data,
       int length,
       PacketPriority priority,
       PacketReliability reliability,
       char orderingChannel,
       const AddressOrGUID systemIdentifier,
       bool broadcast,
       uint32_t forceReceiptNumber = 0
   );

Or with BitStream:

.. code-block:: cpp

   uint32_t Send(
       const BitStream *bitStream,
       PacketPriority priority,
       PacketReliability reliability,
       char orderingChannel,
       const AddressOrGUID systemIdentifier,
       bool broadcast,
       uint32_t forceReceiptNumber = 0
   );

Parameters
----------

data / bitStream
~~~~~~~~~~~~~~~~

The packet data to send.

priority
~~~~~~~~

How urgently to send. See :doc:`reliability-types`.

- ``IMMEDIATE_PRIORITY`` - Send now
- ``HIGH_PRIORITY`` - High priority queue
- ``MEDIUM_PRIORITY`` - Normal priority
- ``LOW_PRIORITY`` - Low priority queue

reliability
~~~~~~~~~~~

Delivery guarantees. See :doc:`reliability-types`.

- ``UNRELIABLE`` - May be lost
- ``RELIABLE`` - Guaranteed delivery
- ``RELIABLE_ORDERED`` - Guaranteed, in-order delivery

orderingChannel
~~~~~~~~~~~~~~~

Channel 0-31 for independent ordering streams. Messages on different channels don't block each other.

systemIdentifier
~~~~~~~~~~~~~~~~

Who to send to:

- ``SystemAddress`` - Send to specific IP:port
- ``RakNetGUID`` - Send to specific peer by GUID
- ``UNASSIGNED_SYSTEM_ADDRESS`` - Used with broadcast

broadcast
~~~~~~~~~

- ``false`` - Send only to ``systemIdentifier``
- ``true`` - Send to everyone EXCEPT ``systemIdentifier``

Examples
--------

Send to One System
~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_GAME_MESSAGE);
   bs.Write(someData);

   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
              targetAddress, false);

Broadcast to All
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Send to everyone (server to all clients)
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
              MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

Broadcast Except One
~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Send to everyone except the sender (relay a message)
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
              senderAddress, true);

Using Ordering Channels
~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Game state updates on channel 0
   peer->Send(&positionUpdate, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
              addr, false);

   // Chat messages on channel 1
   peer->Send(&chatMessage, MEDIUM_PRIORITY, RELIABLE_ORDERED, 1,
              addr, false);

   // Voice data on channel 2 (doesn't need ordering)
   peer->Send(&voiceData, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 2,
              addr, false);

SendList
--------

Send multiple buffers as a single packet:

.. code-block:: cpp

   const char* buffers[3] = { header, payload, footer };
   int lengths[3] = { headerLen, payloadLen, footerLen };

   peer->SendList(buffers, lengths, 3,
                  HIGH_PRIORITY, RELIABLE_ORDERED, 0, addr, false);

Return Value
------------

``Send()`` returns a message number (for ``*_WITH_ACK_RECEIPT`` reliability types):

.. code-block:: cpp

   uint32_t msgNum = peer->Send(&bs, HIGH_PRIORITY,
                                RELIABLE_WITH_ACK_RECEIPT, 0, addr, false);
   // Store msgNum to match with ID_SND_RECEIPT_ACKED later

Common Patterns
---------------

Request-Response
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Client sends request
   MafiaNet::BitStream request;
   request.Write((MafiaNet::MessageID)ID_REQUEST_PLAYER_LIST);
   peer->Send(&request, HIGH_PRIORITY, RELIABLE, 0, serverAddr, false);

   // Server handles and responds
   case ID_REQUEST_PLAYER_LIST: {
       MafiaNet::BitStream response;
       response.Write((MafiaNet::MessageID)ID_PLAYER_LIST);
       response.Write((uint16_t)players.size());
       for (auto& p : players) {
           response.Write(p.name);
       }
       peer->Send(&response, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                  packet->systemAddress, false);
       break;
   }

Periodic Updates
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Send position updates frequently, unreliably
   if (timeSinceLastUpdate > 50) {  // 20 updates/sec
       MafiaNet::BitStream bs;
       bs.Write((MafiaNet::MessageID)ID_POSITION_UPDATE);
       bs.Write(position);
       bs.Write(velocity);

       peer->Send(&bs, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0,
                  MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

       timeSinceLastUpdate = 0;
   }

Best Practices
--------------

1. **Use appropriate reliability**: Don't use RELIABLE_ORDERED for everything.

2. **Use ordering channels**: Separate independent data streams.

3. **Batch small messages**: Combine multiple small updates into one packet.

4. **Rate limit**: Don't send faster than necessary.

5. **Consider bandwidth**: Minimize packet sizes, especially for frequent updates.

See Also
--------

* :doc:`reliability-types` - Priority and reliability options
* :doc:`creating-packets` - How to create packets
* :doc:`receiving-packets` - Receiving packets
