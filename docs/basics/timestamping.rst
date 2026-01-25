Timestamping
============

Timestamps allow synchronization of game events across the network, accounting for latency.

Why Use Timestamps
------------------

Without timestamps, you can't know when an event actually occurred on the remote system. With timestamps, you can:

- Interpolate/extrapolate entity positions
- Synchronize game events (e.g., explosions)
- Implement lag compensation
- Calculate packet age

How Timestamps Work
-------------------

MafiaNet automatically converts timestamps to the receiver's local time using clock synchronization. When you read a timestamp, it's already in your local time frame.

Sending Timestamped Packets
---------------------------

Timestamps must be the first data in the packet:

.. code-block:: cpp

   MafiaNet::BitStream bs;

   // Timestamp must come first
   bs.Write((MafiaNet::MessageID)ID_TIMESTAMP);
   bs.Write(MafiaNet::GetTime());

   // Then your message ID and data
   bs.Write((MafiaNet::MessageID)ID_PLAYER_POSITION);
   bs.Write(position.x);
   bs.Write(position.y);
   bs.Write(position.z);

   peer->Send(&bs, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0, addr, true);

Receiving Timestamped Packets
-----------------------------

.. code-block:: cpp

   MafiaNet::BitStream bs(packet->data, packet->length, false);

   MafiaNet::MessageID firstByte;
   bs.Read(firstByte);

   MafiaNet::Time timestamp = 0;
   bool hasTimestamp = (firstByte == ID_TIMESTAMP);

   if (hasTimestamp) {
       bs.Read(timestamp);
       bs.Read(firstByte);  // Read actual message ID
   }

   // Process based on message type
   switch (firstByte) {
       case ID_PLAYER_POSITION: {
           float x, y, z;
           bs.Read(x);
           bs.Read(y);
           bs.Read(z);

           if (hasTimestamp) {
               // Calculate how old this position is
               MafiaNet::Time age = MafiaNet::GetTime() - timestamp;
               // Extrapolate position based on age
               ExtrapolatePosition(x, y, z, velocity, age);
           }
           break;
       }
   }

Clock Synchronization
---------------------

MafiaNet synchronizes clocks automatically using ping packets. The synchronization is approximate (within a few milliseconds).

Get the clock offset to a remote system:

.. code-block:: cpp

   // Offset from your clock to remote clock
   MafiaNet::Time offset = peer->GetClockDifferential(remoteAddress);

Practical Example: Position Interpolation
-----------------------------------------

.. code-block:: cpp

   struct NetworkedEntity {
       MafiaNet::Time lastUpdateTime;
       float x, y, z;
       float velX, velY, velZ;

       void OnNetworkUpdate(float newX, float newY, float newZ,
                           float newVelX, float newVelY, float newVelZ,
                           MafiaNet::Time packetTime) {
           // Calculate packet age
           MafiaNet::Time age = MafiaNet::GetTime() - packetTime;

           // Extrapolate position to current time
           float ageSeconds = age / 1000.0f;
           x = newX + newVelX * ageSeconds;
           y = newY + newVelY * ageSeconds;
           z = newZ + newVelZ * ageSeconds;

           velX = newVelX;
           velY = newVelY;
           velZ = newVelZ;
           lastUpdateTime = MafiaNet::GetTime();
       }

       void Update(float deltaTime) {
           // Continue extrapolating between updates
           x += velX * deltaTime;
           y += velY * deltaTime;
           z += velZ * deltaTime;
       }
   };

Lag Compensation Example
------------------------

For a shooting game with lag compensation:

.. code-block:: cpp

   void OnShootReceived(MafiaNet::RakNetGUID shooter,
                        float aimX, float aimY, float aimZ,
                        MafiaNet::Time shootTime) {
       // Calculate when the shot was fired
       MafiaNet::Time age = MafiaNet::GetTime() - shootTime;

       // Rewind game state to that time
       GameState historicState = GetHistoricState(age);

       // Perform hit detection at that point in time
       if (CheckHit(shooter, aimX, aimY, aimZ, historicState)) {
           ApplyDamage(historicState.hitPlayer);
       }
   }

Best Practices
--------------

1. **Only timestamp when needed**: Timestamps add overhead. Use them for time-critical data.

2. **Don't timestamp reliable ordered packets**: They may be delayed significantly, making timestamps less useful.

3. **Validate timestamps**: Reject packets with timestamps too far in the future.

4. **Use for interpolation**: Timestamps are most valuable for smooth movement.

.. code-block:: cpp

   // Validate timestamp isn't in the future (with tolerance)
   MafiaNet::Time now = MafiaNet::GetTime();
   if (timestamp > now + 1000) {  // More than 1 second in future
       // Suspicious - possible cheating or clock issue
       return;
   }

See Also
--------

* :doc:`creating-packets` - Adding timestamps to packets
* :doc:`reliability-types` - When to use timestamps
