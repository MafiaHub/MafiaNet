Reliability Types
=================

MafiaNet provides fine-grained control over how packets are delivered through priority and reliability settings.

Packet Priority
---------------

.. code-block:: cpp

   enum class Priority {       // MafiaNet::Priority
       Immediate,  // Sent immediately, not buffered
       High,       // 2 immediate : 1 high ratio
       Medium,     // 2 high : 1 medium ratio
       Low         // 2 medium : 1 low ratio
   };

- **MafiaNet::Priority::Immediate**: Triggers sends immediately, bypassing the send buffer
- **High / Medium / Low**: Buffered and sent in groups at ~10ms intervals

.. note::
   ``Priority`` and ``Reliability`` are scoped ``enum class`` types in namespace
   ``MafiaNet`` (since 0.10.0). They replaced the removed unscoped
   ``PacketPriority`` / ``PacketReliability`` C enums; enumerator order — and
   therefore the wire format — is unchanged.

High priority packets are sent before medium, and medium before low. Packets are never promoted to higher priority over time.

Packet Reliability
------------------

.. code-block:: cpp

   enum class Reliability {        // MafiaNet::Reliability
       Unreliable,
       UnreliableSequenced,
       Reliable,
       ReliableOrdered,
       ReliableSequenced,
       // With acknowledgment receipts
       UnreliableWithAckReceipt,
       ReliableWithAckReceipt,
       ReliableOrderedWithAckReceipt
   };

Unreliable
~~~~~~~~~~

Sent via raw UDP. May arrive out of order or not at all.

**Best for**: Frequently sent data where missing packets don't matter (e.g., position updates sent 60 times/second).

**Pros**: Lowest overhead - no acknowledgment packets needed.

**Cons**: No ordering, packets may be lost, first to be dropped when buffer is full.

UnreliableSequenced
~~~~~~~~~~~~~~~~~~~

Like unreliable, but only the newest packet is accepted. Older packets are dropped.

**Best for**: Data where only the latest value matters (e.g., mouse position).

**Pros**: Low overhead, prevents old data from overwriting new data.

**Cons**: Many packets dropped. Last packet sent may never arrive.

Reliable
~~~~~~~~

Guaranteed delivery via acknowledgment and retransmission.

**Best for**: Important one-off events where order doesn't matter.

**Pros**: Packet will eventually arrive.

**Cons**: Bandwidth overhead from retransmissions. No ordering guarantee.

ReliableOrdered
~~~~~~~~~~~~~~~

Guaranteed delivery in send order. Packets wait for missing earlier packets.

**Best for**: Most game data - chat messages, game state changes, RPC calls.

**Pros**: Packets arrive in order. Easiest to program for.

**Cons**: One late packet can delay many others, causing lag spikes. Use ordering channels to mitigate.

ReliableSequenced
~~~~~~~~~~~~~~~~~

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
   * - Unreliable
     - No
     - No
     - Lowest
     - Frequent position updates
   * - UnreliableSequenced
     - No
     - Yes*
     - Low
     - Mouse/input state
   * - Reliable
     - Yes
     - No
     - Medium
     - Important unordered events
   * - ReliableOrdered
     - Yes
     - Yes
     - Higher
     - Most game logic, chat
   * - ReliableSequenced
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
   peer->Send(&gameState, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, addr, false);
   peer->Send(&chatMsg, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 1, addr, false);

A delayed game state packet won't block chat messages.

Acknowledgment Receipts
-----------------------

Use ``*_WITH_ACK_RECEIPT`` types to be notified when a message is acknowledged:

.. code-block:: cpp

   uint32_t msgId = peer->Send(&bs, MafiaNet::Priority::High,
                               MafiaNet::Reliability::ReliableWithAckReceipt, 0, addr, false);

   // Later, when receiving packets:
   switch (packet->data[0]) {
       case ID_SND_RECEIPT_ACKED: {
           uint32_t ackedId;
           memcpy(&ackedId, packet->data + 1, 4);
           printf("Message %u was delivered\n", ackedId);
           break;
       }
       case ID_SND_RECEIPT_LOSS: {
           // Only for Unreliable*WithAckReceipt reliability types
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
