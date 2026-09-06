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

.. _ordering-channels:

Ordering Channels
-----------------

Ordered and sequenced messages carry an ordering channel, 0 to 31 (``NUMBER_OF_ORDERED_STREAMS``). Each channel is an independent stream: the receiver keeps one ordered index and one sequenced index per channel, and a message is only ever held back or discarded because of other messages on the *same* channel. Plain ``Unreliable`` and ``Reliable`` messages carry no channel; the parameter is ignored for them.

Two rules follow from how the receiver processes a channel, and they decide which traffic may share one.

**A lost ordered message holds back every later sequenced message on its channel.** A sequenced message is stamped with the ordering index that was current when it was sent. If a ``ReliableOrdered`` message on the same channel is lost, every sequenced message sent after it carries an index the receiver has not reached, so the receiver buffers them until the retransmission arrives and then releases them all at once. Position updates sent ``UnreliableSequenced`` on the channel that also carries reliable game events freeze for one retransmission timeout each time an event is lost, then jump.

**A sequenced stream discards by channel, not by object.** ``UnreliableSequenced`` and ``ReliableSequenced`` keep only the newest message on the channel. If updates for many objects share it, one reordered datagram for one object discards every other object's update that was sent before it. Either give each object its own channel, or send plain ``Unreliable`` and order per object yourself with a sequence number or the message timestamp.

The layout that follows:

.. code-block:: cpp

   // Poses: unreliable, alone on channel 0. Nothing reliable ever goes here.
   peer->Send(&pose, MafiaNet::Priority::High, MafiaNet::Reliability::Unreliable, 0, addr, false);

   // State that must arrive: reliable-ordered on its own channel.
   peer->Send(&state, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 1, addr, false);

   // Object creation and the events that name objects share a channel: only
   // messages on the same channel are ordered against each other, and an event
   // must not overtake the creation of the object it refers to.
   peer->Send(&spawn, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 2, addr, false);
   peer->Send(&event, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 2, addr, false);

   // Bulk transfers apart from everything else, so a lost chunk delays only the transfer.
   peer->Send(&chunk, MafiaNet::Priority::Low, MafiaNet::Reliability::ReliableOrdered, 3, addr, false);

Keep reliable classes that reference each other on one channel; keep anything that must never wait on another class on a channel of its own.

Plugins Send on Channel 0
~~~~~~~~~~~~~~~~~~~~~~~~~

Every plugin sends on channel 0 unless told otherwise. An application that keeps its own sequenced stream on channel 0 shares it with all of them:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Plugin
     - Where the channel is set
   * - :doc:`RPC4 <../plugins/rpc4>`
     - The ``orderingChannel`` argument of ``Signal()``, ``Call()`` and ``CallBlocking()``.
   * - :doc:`ReplicaManager3 <../plugins/replica-manager-3>`
     - ``SetDefaultOrderingChannel()`` for construction, destruction and scope messages; ``SerializeParameters::pro[channel].orderingChannel`` inside ``Replica3::Serialize()`` for state.
   * - :doc:`FileListTransfer <../plugins/file-list-transfer>` / :doc:`DirectoryDeltaTransfer <../plugins/directory-delta-transfer>`
     - The ``orderingChannel`` argument of ``FileListTransfer::Send()`` and ``DownloadFromSubdirectory()``; ``DirectoryDeltaTransfer::SetUploadSendParameters()`` on the serving side.
   * - :doc:`RakVoice <../plugins/rakvoice>`
     - ``SetOrderingChannels(frameChannel, controlChannel)``.
   * - :doc:`ReadyEvent <../plugins/ready-event>`
     - ``SetSendChannel()``.
   * - TwoWayAuthentication
     - Fixed on channel 0; its messages are handshake-time only.

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
