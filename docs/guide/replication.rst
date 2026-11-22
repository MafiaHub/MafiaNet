Object replication
==================

Every UDP peer owns a replication manager. An ordinary C++ object can publish
a snapshot, enter and leave a client's streaming range, and accept input from
a server-selected player. Register the same wire type ID and codec version on
each endpoint before connecting.

Replicate a door
----------------

.. code-block:: cpp

   #include "mafianet/peer_interface.h"
   #include "mafianet/snapshot_replica.h"

   struct DoorState {
       bool open{};
       void Write(MafiaNet::BitStream &out) const { out.Write(open); }
       bool Read(MafiaNet::BitStream &in) { return in.Read(open); }
   };
   struct Door : MafiaNet::SnapshotReplica<DoorState> {};

   bool Configure(MafiaNet::RakPeerInterface &peer, MafiaNet::ReplicationRole role) {
       auto &replication = peer.GetReplication();
       return replication.SetRole(role) &&
              replication.RegisterReplica<Door>(MafiaNet::ReplicaTypeId{17}, 1);
   }

Set the host role to ``Server`` and the remote role to ``Client``. On the host,
``replication.Reference(&door)`` borrows the door. Change ``door.state.open``
during gameplay. The usual ``Receive()`` loop drives replication. A joining client
gets a current snapshot and can look it up with ``FindReplica(id)``. ``Destroy(&door)``
removes it from observers and returns sole responsibility for its lifetime to
the game. ``Dereference(&door)`` only ends local tracking. Have the object
outlive its registration.

A receiver allocates each copy with the registered factory. The overload
``RegisterReplica<T>(id, version, create, release)`` supports game pools. The
release callback owns cleanup on a failed decode, scope exit, destruction,
disconnect and shutdown. Store a network ID across ticks and resolve it when
needed. A new visit to the same cell can allocate a different local copy.
For objects already present in both levels, use ``ReferenceStatic(&room, id)``
and implement the construction-existing callbacks on the object.

Spatial streaming
-----------------

.. code-block:: cpp

   auto &grid = replication.GetInterestGrid();
   if (!grid.Configure({{0, 0}, {256, 256}, 64}) ||
       !grid.SetObserverRadius(driver, {63, 20}, 10, 15) ||
       !grid.SetObserverCells(spectator, {{1, 0}}) ||
       !replication.Reference(&vehicle, MafiaNet::SpatialScope{{72, 20}}))
       return false;

The driver sees a vehicle at x=72, retains it at x=77, and loses it at x=79.
The spectator subscribed to cell (1, 0) sees it at all three positions. Move
either endpoint with ``SetPosition(vehicle.GetNetworkID(), point)`` or
``SetObserverPosition(driver, point)``. Each peer can observe a logical world
and dimension through ``SetObserverWorld``. World IDs must match. Dimensions
follow the :doc:`../plugins/virtual-worlds` rule. Use ``SetAudience`` for a game
permission check and ``InvalidateObserver`` when permission data changes.

Coordinates are finite game units. Cells start at the minimum corner and
include their lower edge. The outermost cells include the maximum coordinate.
Invalid moves and reconfigurations return false and retain the prior layout.
A radius observer uses exact distances at both thresholds. Explicit cell
subscriptions are replaced together. A newly connected observer initially
sees eligible ordinary objects. Spatial objects require a subscription or an
eligible authority grant.

``SetObserverLimits(guid, visibleLimit, constructionBudget)`` bounds the
spatial population and the number of new objects sent per tick. Zero means
unlimited. After required objects, spatial selection uses priority, squared
distance and network ID. Incumbents receive a five-percent distance margin.
Older queued entries go first among ordinary construction requests. Read
``GetStatistics(guid)`` for active-session membership, deferred construction,
scope events and payload bytes. ``grid.GetStatistics()`` counts actual
candidate inspections and observer rebuilds.

Client ownership
----------------

The server owns the network ID and lifetime. Register one typed owner-report
codec per vehicle type on every peer. Its version is part of the session
schema. The game receives the sending peer's identity and a checked report.

.. code-block:: cpp

   if (!replication.RegisterOwnerUpdate<Vehicle, DriverInput>(1, &Vehicle::Drive))
       return false;

After admission, the host calls ``SetAuthority(vehicle, driver)``. This brings
the vehicle into scope for that driver, delivers its baseline, and enables
``SendOwnerUpdate(vehicle, input)`` on the client. Check
``HasLocalAuthority(vehicle)`` before sending. The server's ``Drive`` method
validates input and changes the fields the game permits. Other clients receive
canonical state through the usual downstream serializer.

Call ``SetAuthority(vehicle, nextDriver)`` for a handoff,
``RevokeAuthority(vehicle)`` to take back control, or
``CorrectAuthority(vehicle)`` after correcting the authoritative state.
Each grant has its own generation and sequence. A delayed report from an
earlier driver, including an earlier grant to the same player, is discarded.
A grant can extend the streaming radius. Losing world, dimension or audience
eligibility revokes it. Disconnecting releases the grant and keeps the server
object alive. The application chooses whether to destroy a departing avatar.

Delivery and scheduling
-----------------------

Construction, removal, grants and default snapshots use reliable ordered
delivery. Each visit starts with a current baseline. State messages identify
the visit and carry a sequence, tracked separately per channel. Full snapshots
decode into a temporary before replacing the previous state.

After registering a snapshot type, use
``SetSnapshotPolicy(type, ChangeDetection::Explicit, SnapshotDelivery::Replaceable)``
for frequently changing state. Mark changed objects with ``MarkDirty(object)``
or use ``MarkAllDirty()`` after a bulk edit. An entering observer always gets
current state. Replaceable snapshots go out unreliably with application
acknowledgment. A newer snapshot supersedes the pending one. After 250 ms
the sender reissues the latest unconfirmed snapshot on the reliable lifecycle
channel. Both modes progress while the application drives replication.

``SetAutomaticAdmission(false)`` lets the game call ``Admit(guid)`` after its
login checks. Check ``GetPeerStatus(guid)`` for activation or schema rejection.
Use ``SetTickMode(ReplicationTickMode::Manual)`` and call ``Tick()`` after
simulation when the game owns the update schedule. Keep draining ``Receive()``
for connections, inputs and inbound state. For multi-threaded games, guard
shared game state and grid edits with ``ReplicationManager::WorldGuard`` on the
simulation thread. Incoming state callbacks run on the receiving thread under
the source connection's lock. Queue world mutations from those callbacks.

Moving an RM3 game to the built-in service
------------------------------------------

===============================  ============================================================
Game requirement                 Built-in API
===============================  ============================================================
Peer setup                       ``GetReplication()`` and ``SetRole()``
Object factory and pooling       ``RegisterReplica<T>(id, version, create, release)``
Recipient visibility             ``GetInterestGrid()`` and the replica's construction hooks
Custom serialization             ``Replica3`` callbacks and per-channel send parameters
Application tick / worker pool   ``SetTickMode()``, ``SetTickCallbacks()``, ``SetSerializeExecutor()``
Application change queue         ``MarkDirty()`` or ``SetSerializationList()``
Connection notification          ``SetConnectionCallback()``
Symmetric peer creation          ``ReplicationRole::Peer`` on both ends
===============================  ============================================================

Build and run ``Samples/Replication/Replication`` for a host, two observers,
a vehicle handoff and a room change. ``Samples/ReplicaManager3`` demonstrates
the lower-level callbacks.
