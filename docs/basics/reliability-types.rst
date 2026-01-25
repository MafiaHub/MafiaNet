Reliability Types
=================

MafiaNet provides fine-grained control over how packets are delivered through priority and reliability settings.

Packet Priority
---------------

.. code-block:: cpp

   enum PacketPriority {
       IMMEDIATE_PRIORITY,  // Sent immediately, not buffered
       HIGH_PRIORITY,       // 2 immediate : 1 high ratio
       MEDIUM_PRIORITY,     // 2 high : 1 medium ratio
       LOW_PRIORITY         // 2 medium : 1 low ratio
   };

- **IMMEDIATE_PRIORITY**: Triggers sends immediately, bypassing the send buffer
- **HIGH/MEDIUM/LOW**: Buffered and sent in groups at ~10ms intervals

High priority packets are sent before medium, and medium before low. Packets are never promoted to higher priority over time.

Packet Reliability
------------------

.. code-block:: cpp

   enum PacketReliability {
       UNRELIABLE,
       UNRELIABLE_SEQUENCED,
       RELIABLE,
       RELIABLE_ORDERED,
       RELIABLE_SEQUENCED,
       // With acknowledgment receipts
       UNRELIABLE_WITH_ACK_RECEIPT,
       UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT,
       RELIABLE_WITH_ACK_RECEIPT,
       RELIABLE_ORDERED_WITH_ACK_RECEIPT,
       RELIABLE_SEQUENCED_WITH_ACK_RECEIPT
   };

UNRELIABLE
~~~~~~~~~~

Sent via raw UDP. May arrive out of order or not at all.

**Best for**: Frequently sent data where missing packets don't matter (e.g., position updates sent 60 times/second).

**Pros**: Lowest overhead - no acknowledgment packets needed.

**Cons**: No ordering, packets may be lost, first to be dropped when buffer is full.

UNRELIABLE_SEQUENCED
~~~~~~~~~~~~~~~~~~~~

Like unreliable, but only the newest packet is accepted. Older packets are dropped.

**Best for**: Data where only the latest value matters (e.g., mouse position).

**Pros**: Low overhead, prevents old data from overwriting new data.

**Cons**: Many packets dropped. Last packet sent may never arrive.

RELIABLE
~~~~~~~~

Guaranteed delivery via acknowledgment and retransmission.

**Best for**: Important one-off events where order doesn't matter.

**Pros**: Packet will eventually arrive.

**Cons**: Bandwidth overhead from retransmissions. No ordering guarantee.

RELIABLE_ORDERED
~~~~~~~~~~~~~~~~

Guaranteed delivery in send order. Packets wait for missing earlier packets.

**Best for**: Most game data - chat messages, game state changes, RPC calls.

**Pros**: Packets arrive in order. Easiest to program for.

**Cons**: One late packet can delay many others, causing lag spikes. Use ordering channels to mitigate.

RELIABLE_SEQUENCED
~~~~~~~~~~~~~~~~~~

Guaranteed delivery, but only latest packet is kept. Old packets are dropped even if they arrive.

**Best for**: Continuous streams where you need reliability but only care about the latest value.

**Pros**: Reliable, ordered, no waiting for old packets.

**Cons**: Bandwidth overhead - reliable packets are sent even though old ones are discarded.

Comparison Table
----------------

.. list-table::
   :header-rows: 1
   :widths: 25 15 15 15 30

   * - Type
     - Reliable
     - Ordered
     - Bandwidth
     - Use Case
   * - UNRELIABLE
     - No
     - No
     - Lowest
     - Frequent position updates
   * - UNRELIABLE_SEQUENCED
     - No
     - Yes*
     - Low
     - Mouse/input state
   * - RELIABLE
     - Yes
     - No
     - Medium
     - Important unordered events
   * - RELIABLE_ORDERED
     - Yes
     - Yes
     - Higher
     - Most game logic, chat
   * - RELIABLE_SEQUENCED
     - Yes
     - Yes*
     - Higher
     - Continuous reliable streams

\* Sequenced types drop older packets rather than waiting for them.

Ordering Channels
-----------------

Messages can be sent on ordering channels 0-31. Messages on different channels are ordered independently:

.. code-block:: cpp

   // Game state on channel 0, chat on channel 1
   peer->Send(&gameState, HIGH_PRIORITY, RELIABLE_ORDERED, 0, addr, false);
   peer->Send(&chatMsg, HIGH_PRIORITY, RELIABLE_ORDERED, 1, addr, false);

A delayed game state packet won't block chat messages.

Acknowledgment Receipts
-----------------------

Use ``*_WITH_ACK_RECEIPT`` types to be notified when a message is acknowledged:

.. code-block:: cpp

   uint32_t msgId = peer->Send(&bs, HIGH_PRIORITY,
                               RELIABLE_WITH_ACK_RECEIPT, 0, addr, false);

   // Later, when receiving packets:
   switch (packet->data[0]) {
       case ID_SND_RECEIPT_ACKED: {
           uint32_t ackedId;
           memcpy(&ackedId, packet->data + 1, 4);
           printf("Message %u was delivered\n", ackedId);
           break;
       }
       case ID_SND_RECEIPT_LOSS: {
           // Only for UNRELIABLE_*_WITH_ACK_RECEIPT
           uint32_t lostId;
           memcpy(&lostId, packet->data + 1, 4);
           printf("Message %u was probably lost\n", lostId);
           break;
       }
   }

.. note::
   ``ID_SND_RECEIPT_LOSS`` could mean the message was lost, the ack was lost, or the ack arrived late (ping spike).

Connection Detection
--------------------

.. warning::
   Reliable packet transmission is required for lost connection detection. If you only send unreliable packets, implement your own keepalive/timeout mechanism.

See Also
--------

* :doc:`sending-packets` - How to send packets
* :doc:`../guide/concepts` - Core networking concepts
