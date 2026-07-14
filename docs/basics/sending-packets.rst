Sending Packets
===============

Once you've created a packet, send it using ``Send()`` or ``SendList()``. If
you use the typed message dispatcher, ``Peer::send()`` / ``Peer::broadcast()``
serialize and send a message struct in one call — see
:ref:`typed-send-broadcast` below.

The Send Function
-----------------

.. code-block:: cpp

   uint32_t Send(
       const char *data,
       int length,
       MafiaNet::Priority priority,
       MafiaNet::Reliability reliability,
       char orderingChannel,
       const AddressOrGUID systemIdentifier,
       bool broadcast,
       uint32_t forceReceiptNumber = 0
   );

Or with BitStream:

.. code-block:: cpp

   uint32_t Send(
       const BitStream *bitStream,
       MafiaNet::Priority priority,
       MafiaNet::Reliability reliability,
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

- ``MafiaNet::Priority::Immediate`` - Send now
- ``MafiaNet::Priority::High`` - High priority queue
- ``MafiaNet::Priority::Medium`` - Normal priority
- ``MafiaNet::Priority::Low`` - Low priority queue

reliability
~~~~~~~~~~~

Delivery guarantees. See :doc:`reliability-types`.

- ``MafiaNet::Reliability::Unreliable`` - May be lost
- ``MafiaNet::Reliability::Reliable`` - Guaranteed delivery
- ``MafiaNet::Reliability::ReliableOrdered`` - Guaranteed, in-order delivery

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

   peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0,
              targetAddress, false);

Broadcast to All
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Send to everyone (server to all clients)
   peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0,
              MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

Broadcast Except One
~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Send to everyone except the sender (relay a message)
   peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0,
              senderAddress, true);

Using Ordering Channels
~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Game state updates on channel 0
   peer->Send(&positionUpdate, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0,
              addr, false);

   // Chat messages on channel 1
   peer->Send(&chatMessage, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 1,
              addr, false);

   // Voice data on channel 2 (doesn't need ordering)
   peer->Send(&voiceData, MafiaNet::Priority::High, MafiaNet::Reliability::UnreliableSequenced, 2,
              addr, false);

.. _typed-send-broadcast:

Typed Send / Broadcast
----------------------

With the RAII ``Peer`` handle and a ``Dispatcher`` (see
:doc:`receiving-packets`), a registered message struct can be serialized and
sent in a single call — no manual ``BitStream`` or identifier byte:

.. code-block:: cpp

   struct ChatMessage {
       std::string text; int channel;
       template <class Ar> void serialize(Ar& ar) { ar & text & channel; }
   };

   MafiaNet::Dispatcher d;
   d.on<ChatMessage>(OnChatMessage);   // registers ChatMessage's identifier

   peer.send(d, ChatMessage{"hi", 0}, targetAddress);       // one system
   peer.broadcast(d, ChatMessage{"hi", 0});                 // everyone connected

Defaults are ``Priority::High``, ``Reliability::ReliableOrdered``, ordering
channel 0 — each overridable per call:

.. code-block:: cpp

   peer.send(d, PositionUpdate{x, y, z}, target,
             MafiaNet::Reliability::UnreliableSequenced);

The destination accepts a ``SystemAddress`` or a ``RakNetGUID`` (implicitly
converted to ``AddressOrGUID``); both peers must agree on the message's
identifier via the dispatcher's registry (see :doc:`../api/core`). The raw
``Send()`` remains fully supported.

SendList
--------

Send multiple buffers as a single packet:

.. code-block:: cpp

   const char* buffers[3] = { header, payload, footer };
   int lengths[3] = { headerLen, payloadLen, footerLen };

   peer->SendList(buffers, lengths, 3,
                  MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, addr, false);

Return Value
------------

``Send()`` returns a message number (for ``*_WITH_ACK_RECEIPT`` reliability types):

.. code-block:: cpp

   uint32_t msgNum = peer->Send(&bs, MafiaNet::Priority::High,
                                MafiaNet::Reliability::ReliableWithAckReceipt, 0, addr, false);
   // Store msgNum to match with ID_SND_RECEIPT_ACKED later

Common Patterns
---------------

Request-Response
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Client sends request
   MafiaNet::BitStream request;
   request.Write((MafiaNet::MessageID)ID_REQUEST_PLAYER_LIST);
   peer->Send(&request, MafiaNet::Priority::High, MafiaNet::Reliability::Reliable, 0, serverAddr, false);

   // Server handles and responds
   case ID_REQUEST_PLAYER_LIST: {
       MafiaNet::BitStream response;
       response.Write((MafiaNet::MessageID)ID_PLAYER_LIST);
       response.Write((uint16_t)players.size());
       for (auto& p : players) {
           response.Write(p.name);
       }
       peer->Send(&response, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0,
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

       peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::UnreliableSequenced, 0,
                  MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

       timeSinceLastUpdate = 0;
   }

Best Practices
--------------

1. **Use appropriate reliability**: Don't use ``MafiaNet::Reliability::ReliableOrdered`` for everything.

2. **Use ordering channels**: Separate independent data streams.

3. **Batch small messages**: Combine multiple small updates into one packet.

4. **Rate limit**: Don't send faster than necessary.

5. **Consider bandwidth**: Minimize packet sizes, especially for frequent updates.

See Also
--------

* :doc:`reliability-types` - Priority and reliability options
* :doc:`creating-packets` - How to create packets
* :doc:`receiving-packets` - Receiving packets
