Programming Tips
================

This guide provides best practices and tips for effective MafiaNet development.

Connection Management
---------------------

**Always handle all disconnect scenarios:**

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_DISCONNECTION_NOTIFICATION:
           // Clean disconnect - save player data
           SavePlayerData(packet->guid);
           RemovePlayer(packet->guid);
           break;

       case ID_CONNECTION_LOST:
           // Timeout - may need reconnection logic
           MarkPlayerDisconnected(packet->guid);
           StartReconnectionTimer(packet->guid);
           break;
   }

**Don't forget to deallocate packets:**

.. code-block:: cpp

   // BAD - memory leak
   MafiaNet::Packet* p = peer->Receive();
   if (p->data[0] == ID_SOMETHING) { return; }

   // GOOD - always deallocate
   MafiaNet::Packet* p = peer->Receive();
   if (p) {
       ProcessPacket(p);
       peer->DeallocatePacket(p);
   }

BitStream Best Practices
------------------------

**Use appropriate data types:**

.. code-block:: cpp

   // BAD - wastes bandwidth
   bs.Write((int)health);  // 32 bits for 0-100 value

   // GOOD - right-sized
   bs.Write((unsigned char)health);  // 8 bits

   // BETTER - even smaller
   bs.WriteBits((unsigned char*)&health, 7);  // 7 bits for 0-127

**Reset BitStream read position for re-reading:**

.. code-block:: cpp

   MafiaNet::BitStream bs(packet->data, packet->length, false);
   bs.IgnoreBytes(1);  // Skip message ID

   // Read once
   int value1;
   bs.Read(value1);

   // Reset to read again
   bs.ResetReadPointer();
   bs.IgnoreBytes(1);

**Serialize in consistent order:**

.. code-block:: cpp

   // Write order MUST match read order
   void WritePlayer(BitStream& bs, Player& p) {
       bs.Write(p.id);
       bs.Write(p.x);
       bs.Write(p.y);
       bs.Write(p.health);
   }

   void ReadPlayer(BitStream& bs, Player& p) {
       bs.Read(p.id);
       bs.Read(p.x);
       bs.Read(p.y);
       bs.Read(p.health);
   }

Reliability Selection
---------------------

Choose the right reliability for each message type:

+---------------------+------------------------+----------------------+
| Data Type           | Reliability            | Why                  |
+=====================+========================+======================+
| Position updates    | UNRELIABLE_SEQUENCED   | Latest matters most  |
+---------------------+------------------------+----------------------+
| Health changes      | RELIABLE               | Must arrive          |
+---------------------+------------------------+----------------------+
| Chat messages       | RELIABLE_ORDERED       | Order matters        |
+---------------------+------------------------+----------------------+
| Spawn events        | RELIABLE_ORDERED       | Critical + ordered   |
+---------------------+------------------------+----------------------+
| Input commands      | UNRELIABLE_SEQUENCED   | Stale input useless  |
+---------------------+------------------------+----------------------+

Plugin Architecture
-------------------

**Order matters for plugin attachment:**

.. code-block:: cpp

   // Plugins process packets in attachment order
   peer->AttachPlugin(&authPlugin);      // First: authenticate
   peer->AttachPlugin(&messageFilter);   // Second: filter
   peer->AttachPlugin(&gamePlugin);      // Third: game logic

**Use plugin callbacks appropriately:**

.. code-block:: cpp

   class MyPlugin : public MafiaNet::PluginInterface2 {
       // Called before packet reaches application
       PluginReceiveResult OnReceive(Packet* packet) override {
           if (ShouldHandlePacket(packet)) {
               // RR_STOP_PROCESSING_AND_DEALLOCATE - handled, stop
               // RR_CONTINUE_PROCESSING - let others see it
               // RR_STOP_PROCESSING - handled, don't deallocate
           }
           return RR_CONTINUE_PROCESSING;
       }
   };

Performance Tips
----------------

**Batch sends when possible:**

.. code-block:: cpp

   // Instead of many small sends
   for (auto& update : updates) {
       peer->Send(&update, ...);  // Many syscalls
   }

   // Combine into one BitStream
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_BATCH_UPDATE);
   bs.Write((unsigned short)updates.size());
   for (auto& update : updates) {
       update.Serialize(bs);
   }
   peer->Send(&bs, ...);  // One syscall

**Use ordering channels wisely:**

.. code-block:: cpp

   // Different channels don't block each other
   peer->Send(&positionData, MED, RELIABLE_ORDERED, 0, ...);  // Channel 0
   peer->Send(&chatMessage, MED, RELIABLE_ORDERED, 1, ...);   // Channel 1
   // Chat won't wait for position packets

Debugging Tips
--------------

* Enable PacketLogger during development
* Use GetStatistics() to monitor connection health
* Log all connection state changes
* Test with artificial latency and packet loss

See Also
--------

* :doc:`faq` - Common questions
* :doc:`../advanced/congestion-control` - Bandwidth management
* :doc:`../advanced/debugging-disconnects` - Troubleshooting
