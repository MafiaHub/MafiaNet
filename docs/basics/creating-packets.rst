Creating Packets
================

Packets are the units of data sent over the network. MafiaNet provides two ways to create packets: using structures or using BitStreams.

Message IDs
-----------

Every packet starts with a message ID byte. MafiaNet reserves IDs 0-134 for internal use. Your custom messages should start at ``ID_USER_PACKET_ENUM``:

.. code-block:: cpp

   // In a shared header file
   enum GameMessages {
       ID_PLAYER_POSITION = ID_USER_PACKET_ENUM,
       ID_PLAYER_SHOOT,
       ID_CHAT_MESSAGE,
       // ... more message types
   };

Using Structs
-------------

The simplest way to create packets:

.. code-block:: cpp

   #pragma pack(push, 1)  // Ensure no padding
   struct PlayerPositionPacket {
       unsigned char messageId;  // Must be first
       float x, y, z;
       float yaw, pitch;
   };
   #pragma pack(pop)

   // Sending
   PlayerPositionPacket packet;
   packet.messageId = ID_PLAYER_POSITION;
   packet.x = player.x;
   packet.y = player.y;
   packet.z = player.z;
   packet.yaw = player.yaw;
   packet.pitch = player.pitch;

   peer->Send((char*)&packet, sizeof(packet),
              HIGH_PRIORITY, RELIABLE_ORDERED, 0, address, false);

   // Receiving
   PlayerPositionPacket* received = (PlayerPositionPacket*)packet->data;
   player.x = received->x;
   player.y = received->y;
   // ...

.. warning::
   Struct-based packets don't handle endian differences between platforms. Use BitStream for cross-platform compatibility.

Using BitStream
---------------

More flexible and handles endianness:

.. code-block:: cpp

   // Sending
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_PLAYER_POSITION);
   bs.Write(player.x);
   bs.Write(player.y);
   bs.Write(player.z);
   bs.Write(player.yaw);
   bs.Write(player.pitch);

   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, address, false);

   // Receiving
   MafiaNet::BitStream bsIn(packet->data, packet->length, false);
   bsIn.IgnoreBytes(1);  // Skip message ID

   float x, y, z, yaw, pitch;
   bsIn.Read(x);
   bsIn.Read(y);
   bsIn.Read(z);
   bsIn.Read(yaw);
   bsIn.Read(pitch);

Timestamps
----------

To synchronize timing across the network, use timestamps:

.. code-block:: cpp

   // Sending
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_TIMESTAMP);
   bs.Write(MafiaNet::GetTime());  // Current time
   bs.Write((MafiaNet::MessageID)ID_PLAYER_POSITION);
   bs.Write(player.x);
   // ...

   // Receiving
   MafiaNet::BitStream bsIn(packet->data, packet->length, false);
   MafiaNet::MessageID msgId;
   bsIn.Read(msgId);

   if (msgId == ID_TIMESTAMP) {
       MafiaNet::Time timestamp;
       bsIn.Read(timestamp);
       bsIn.Read(msgId);  // Read actual message ID

       // timestamp is now in your local time
       MafiaNet::Time packetAge = MafiaNet::GetTime() - timestamp;
   }

.. note::
   ``ID_TIMESTAMP`` must be the first byte. MafiaNet automatically converts the timestamp to the receiver's local time.

Compression Techniques
----------------------

Use less bandwidth with compression:

.. code-block:: cpp

   // For integers that are usually small
   bs.WriteCompressed(health);  // 100 uses ~1 byte, not 4

   // For normalized vectors (length = 1)
   bs.WriteNormVector(dirX, dirY, dirZ);

   // For quaternions
   bs.WriteNormQuat(qw, qx, qy, qz);

   // For floats with limited precision needs
   bs.WriteFloat16(value, minValue, maxValue);

   // For integers within a known range
   bs.WriteBitsFromIntegerRange(value, minValue, maxValue);

Strings
-------

.. code-block:: cpp

   // Using RakString (more efficient)
   MafiaNet::RakString str = "Hello, World!";
   bs.Write(str);

   // Or C-strings
   bs.Write("Hello, World!");

   // Reading
   MafiaNet::RakString received;
   bsIn.Read(received);

Variable-Length Data
--------------------

For arrays or variable-length data:

.. code-block:: cpp

   // Write count first, then elements
   bs.Write((uint16_t)items.size());
   for (const auto& item : items) {
       bs.Write(item);
   }

   // Reading
   uint16_t count;
   bsIn.Read(count);
   for (uint16_t i = 0; i < count; i++) {
       ItemType item;
       bsIn.Read(item);
       items.push_back(item);
   }

Best Practices
--------------

1. **Always check read success**:

   .. code-block:: cpp

      if (!bsIn.Read(value)) {
          // Packet was truncated/corrupted
          return;
      }

2. **Use smallest types possible**: ``uint8_t`` for small values, ``uint16_t`` for medium.

3. **Consider delta compression**: Send only what changed since last update.

4. **Group related data**: Send position and velocity together rather than separately.

5. **Use compression**: ``WriteCompressed()`` and specialized methods save bandwidth.

See Also
--------

* :doc:`bitstreams` - Detailed BitStream documentation
* :doc:`sending-packets` - How to send packets
* :doc:`timestamping` - Timestamp synchronization
