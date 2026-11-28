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
on the receiver. The sender may also use an ordinary reference whose construction
query returns ``RM3CS_ALREADY_EXISTS_REMOTELY``. Implement the existing-object
callbacks for a binding baseline, or return
``RM3CS_ALREADY_EXISTS_REMOTELY_DO_NOT_CONSTRUCT`` to start streaming state
to the pre-existing object without a baseline.

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

Work follows the change, not the population. Moving an object rebuilds only
the observers it can affect: radius observers that hold it or now have it
within their enter radius, and explicit-cell observers when it changes cell.
An observer that is not rebuilt keeps its previous visible list, order
included. An object referenced without a scope and later passed to
``grid.Track()`` becomes spatial, and ``grid.Remove()`` makes it ordinary
again. Each connection's update visits only the ordinary objects the grid does
not track, plus its own visible set.

Categories and ranges
~~~~~~~~~~~~~~~~~~~~~

Set ``SpatialScope::category`` to group objects, then call
``SetCategoryLimit(category, maxCount)`` to cap how many objects of that
category each observer sees. The highest priority objects are kept first, then
the nearest, with the same five-percent distance margin for incumbents. Category 0 is uncategorised and never limited. Zero as
``maxCount`` removes a limit.

``SpatialScope::range`` lets an object stream at its own distance. By default
an object is seen within the larger of its range and the observer's enter
radius, so a range can only widen interest. For a category passed to
``SetCategoryUsesEntityRange(category, true)`` the range replaces the observer's
radius instead, so dense scenery can stream nearer than everything else. An
object whose range is 0 keeps the observer's enter and leave radii, even in such
a category. The observer's leave margin applies to both.

Look-ahead
~~~~~~~~~~

``SetLookaheadSeconds(seconds)`` and ``SetObserverVelocity(guid, velocity)``
give a radius observer a second focus point at its position plus velocity times
seconds. An object is in range when it is within range of either point, so a
fast driver sees the road ahead. A focus point less than 5 units from the
observer is ignored; 5 units or more is used. Look-ahead widens the area
considered for visibility, but the visible limit and category limits still apply
afterwards, so a newly reachable object can displace one that was visible.
While look-ahead is off, ``SetObserverVelocity`` only stores the velocity and
does not mark the observer for a rebuild, so a server can report every player's
velocity each tick at no cost; turning look-ahead on uses the stored velocities.

Dependencies and always-visible objects
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``SetDependency(child, parent)`` makes the child visible whenever the parent
is, wherever the child is. Chains are followed. A link that would make a cycle,
names an untracked object or links an object to itself returns false. Pass ``UNASSIGNED_NETWORK_ID`` as
parent to clear the link. Removing an object drops its links.
``SetAlwaysVisible(id, true)`` shows an object to every observer that may see
it, whatever its position.

Required objects, dependency children and always-visible objects are added on
top of the limits: they do not count toward the ``visibleLimit`` argument of
``SetObserverLimits`` or toward a category limit. World, dimension and audience rules still apply to all of them.
``InterestStatistics`` reports ``alwaysVisiblePulls``, ``dependencyPulls`` and
``categoryCapped``.

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

Bound how often a driver can report with
``SetOwnerUpdateRateLimit(vehicleType, 60, 90)`` on the server. Each grant gets
its own budget of 90 reports, refilled at 60 per second. Reports beyond it are
dropped before ``Drive`` runs and counted in ``GetStatistics(guid).ownerUpdatesDropped``.
A dropped report does not advance the grant's sequence, so a correction never
claims to have processed it. The limit is local server policy, not part of the
session schema, and can change at any time. Zero accepts every report.

Delegated simulation
--------------------

The server can choose which client simulates each object, instead of calling
``SetAuthority`` itself. Opt an object in with a policy. It must be tracked in
the interest grid, which gives its position, and its type needs an owner
report.

.. code-block:: cpp

   MafiaNet::DelegationPolicy policy; // acquire 100, release 140, weight 1, grace 5000 ms
   policy.weight = 2;
   if (!replication.SetDelegation(pedestrian, &policy))
       return false;
   replication.SetDelegationCallback([](MafiaNet::NetworkID id, MafiaNet::PeerGuid previous,
                                        MafiaNet::PeerGuid current, MafiaNet::DelegationChange change)
   {
       // Acquired, Migrated or Dormant: mirror the new simulator in the game.
   });

**Election.** The candidates are the Active radius observers that
``SetAuthority`` would accept for the object: same world, a dimension that can
see it, and an audience that allows it. The current holder is kept while it is
within ``releaseRange``; a challenger must be within ``acquireRange``. With
``proximitySteal`` on, a nearer challenger takes over only when its squared
distance beats 0.72 times the holder's (``kDelegationStickyDiscount``, about 15%
nearer in distance), so a holder is not swapped for a client at the same
distance. Nobody in range leaves the object dormant: the server holds it and its
replicated state stays as it was until a client reports again. The pure rule is
public as ``ElectSimulator`` for tests and tools. Each object is re-elected about
once per ``SetDelegationInterval`` (500 ms by default), spread over ticks by its
network ID, inside ``Tick`` after the interest rebuild. The handover itself is
``SetAuthority``, so the epoch, revocation and eligibility rules above apply
unchanged.

**Capacity.** ``SetClientCapacity(maxWeight)`` and
``SetClientCapacity(guid, maxWeight)`` bound the summed weight a client holds
(0 is unlimited). A client at capacity keeps what it holds and takes nothing
new. ``GetDelegatedLoad(guid)`` reads its load.

**Pin and suspend.** ``Pin(object, guid)`` keeps the object on that client
whatever the ranges say, ``Pin(object, UNASSIGNED_PEER_GUID)`` keeps it dormant,
and ``Unpin`` returns it to the election. ``Suspend(guid)`` releases everything
the delegation gave the client and keeps it out of elections until ``Resume``
or its disconnect. A disconnect re-elects what the client held at once.

**Decline.** A client calls ``DeclineAuthority(copy, true)`` to ask not to
simulate an object, for example while its own game is loading, and ``false`` to
withdraw. The server passes it over and takes the object from it if it holds
it. A decline ends when the object leaves that client's visible set or either
side disconnects. A pin overrides a decline without clearing it. Decline
changes re-elect at most once every 100 ms.

**Release veto.** ``SetReleaseVeto(canRelease)`` lets the game postpone a
handover away from a holder, for example while a vehicle is mid-jump.
``canRelease(id, holder)`` returning false postpones it for at most the
policy's ``graceMs`` from the first refusal; after that the handover happens
anyway. It is not asked for a disconnect, a suspension or a holder that
declined. It runs during ``Tick`` with the replication world guard held, so it
must answer from game state and must not change replication state.

**Groups.** ``SetDelegationGroup(trailer, &truck)`` keeps a member with its
leader's simulator. Links chain to the group's root; a cycle returns false.
Members always have the root's holder, whatever the root's mode. A delegated
root is elected once for the group, with its policy and position and the
group's summed weight, and a client must be eligible for every member to win.
A member's own policy is kept, but only its weight counts while it is grouped;
a member without a policy weighs 1. A handover is all or nothing: if a grant is
refused, the ones already made in that pass are undone and the group is retried
at its next pass. In such an elected group, pinning or declining any member acts
on the whole group, suspending the holder releases the whole group, and the veto
is asked once, with the root's ID.

The members of a root the game owns by hand (not delegated, granted with
``SetAuthority``) follow its holder at the next ``Tick``, and go dormant while
that client cannot hold every one of them (ineligible or suspended). The root's
grant stays the game's: such a group cannot be pinned, declines do not move it,
and a suspension releases only its members. The leader counts toward its
holder's load and capacity like the members.

The link also sets ``InterestGrid::SetDependency(member, leader)``, so a client
that sees the leader sees its members. Pass ``nullptr`` as the leader to
unlink; the grid dependency is cleared too if it still names that leader.
Unlinking a member, or removing its leader, leaves the member with its holder:
it then follows its own policy, leads its own followers, or is the game's
again.

**Timing.** ``Pin``, ``Unpin`` and ``Suspend`` take effect at the next
``Tick``, never inside the call. A newly delegated object waits for its turn
in the schedule. The delegation
callback runs after the change is applied, outside every MafiaNet lock, in the
order the changes were made. A pass asked for from inside it waits for the
next ``Tick``. A ``SetAuthority`` or ``RevokeAuthority`` on a delegated object
is re-elected at the next ``Tick``; pin it, or opt it out with
``SetDelegation(object, nullptr)``, to choose its holder yourself. Opting out
leaves the current holder in place.

``GetDelegationStatistics()`` reports ``simulated`` and ``dormant`` objects
(delegated objects, group members and the leaders of hand-owned groups),
``elections`` run, ``handovers`` (one per grant the delegation changed),
``refusals`` and ``forced`` handovers of the veto, and ``declines`` recorded.

Scene lifecycle
---------------

Install a receiving peer's ``SetReplicaCallback`` to attach scene objects and input
controllers. Each event carries the network ID and the source peer. At
``EnteredScope``, ``FindReplica(event.id)`` resolves the initialized copy.
``AuthorityGranted`` and ``AuthorityCorrected`` follow application of the
server baseline. A correction retains local control. ``AuthorityLost``
precedes a removal when the object still has a grant.

``LeftScope`` ends one visit and allows re-entry with the same ID. ``Destroyed``
ends a visible object's server lifetime. A client outside scope receives no
second removal. ``Released`` reports an explicit local ``ReleaseRemote``, and
``Disconnected`` ends remaining visits from a departing peer. Downloaded
copies have already been returned to the registered pool when the removal
callback runs. A static binding stays owned by the application.

Callbacks run on the thread processing the transition, under the replication
world guard. A callback can submit input, release a copy or replace the
callback. Events generated by the handler are delivered after it returns.
Pass an empty callback to unsubscribe before its captured state expires.

Delivery and scheduling
-----------------------

Construction, removal, grants and default snapshots use reliable ordered
delivery. Lifecycle records use ordering channel 0. A custom serializer can
set ``SerializeParameters::pro[channel].orderingChannel`` for state.
A state channel accepts up to 1 MiB. A larger write increments
``Replica3::GetRejectedStateCount()`` and leaves the previous send cache
intact. Correct the payload and mark the object again when using explicit tracking.
Each dynamic visit starts with a current baseline. State messages identify
the visit and carry a sequence, tracked separately per channel. Full snapshots
decode into a temporary before replacing the previous state.

Within each tick the service batches consecutive state updates for the same
recipient, priority, reliability and ordering channel. A batch holds up to
32 records and 1,200 bytes, with a lower ceiling on smaller negotiated MTUs.
Each record identifies the object and its current visit, and preserves the
exact payload bit length. Immediate, sequenced and receipt-bearing sends keep individual
message boundaries, as do oversized records. A single pending update goes
out in the same tick.

After registering a snapshot type, use
``SetSnapshotPolicy(type, ChangeDetection::Explicit, SnapshotDelivery::Replaceable)``
for frequently changing state. Mark changed objects with ``MarkDirty(object)``
or use ``MarkAllDirty()`` after a bulk edit. An entering observer always gets
current state. Replaceable snapshots go out unreliably with application
acknowledgment. In replication wire version 7, the handshake exchanges a short
receive-context number in each direction on encrypted connections. Snapshot
updates and acknowledgments carry the receiver's context. A reconnect gets a
fresh context, and the encrypted transport authenticates the datagram. Plaintext
connections carry both full session tokens on each message.

Snapshots address an object by the visit handle assigned when it enters scope. A batch of consecutive snapshot
sequences with the same timestamp carries the first sequence and timestamp once.
The receiver confirms one applied sequence with a single receipt. To confirm
several it sends a base sequence and a 32-bit mask. Both go out at the end
of the incoming message, including when ticking manually. A clear
bit leaves that snapshot eligible for repair. Each reply fits the reverse
path's unreliable datagram budget. For each object, a newer update supersedes
only the channels it carries. The sender initially waits 250 ms before reissuing
still-unconfirmed channels on the reliable lifecycle channel. Each observer then
gets its own repair delay based on the time from sending a snapshot to processing
its acknowledgment and how much that time varies. The delay is bounded to
32–2000 ms and repairs run on the next replication tick after the deadline.
If the sampled snapshot needed a reliable repair, its reply backs off the
next delay, since either transmission may have produced it. A refused
transport enqueue of a handshake, grant, revocation or
removal is retried on a subsequent tick. ``SendOwnerUpdate`` returns false
when the report could not be queued, allowing the client to retry.
Both modes progress while the application drives replication.

A state callback can request peer shutdown. Delivery stops at that record,
and the peer shuts down after the callback releases its locks and before
the enclosing receive call returns.

``SetAutomaticAdmission(false)`` lets the game call ``Admit(guid)`` after its
login checks. Check ``GetPeerStatus(guid)`` for activation or schema rejection.
Use ``SetTickMode(ReplicationTickMode::Manual)`` and call ``Tick()`` after
simulation when the game owns the update schedule. Keep draining ``Receive()``
for connections, inputs and inbound state. For multi-threaded games, guard
replica membership and grid edits with ``ReplicationManager::WorldGuard``.
Incoming state callbacks run on the receiving thread under the source
connection's lock and can overlap the simulation thread. Read and apply
``SnapshotReplica::state`` on one thread, or use a custom serializer that
hands decoded state to the simulation through a synchronized queue. Queue
world mutations from state callbacks. Grid audience predicates are read-only.
An attempt to mutate or reevaluate the same grid from its predicate throws
``std::logic_error``.

Observer update rates
---------------------

On an active connection, use ``SetUpdateInterval(observer, vehicle, 4)`` to
consider a visible vehicle once every four serialization passes for that
observer. One is the default. ``SetObserverStateBudget(observer, 768)``
allows 768 bytes of ordinary state per serialization pass. Zero removes the
budget. Both setters return false if the observer is unknown. Intervals are
local to the observer and replica; the connection drops its settings at
disconnect. Grid visibility and construction limits still apply.

The scheduler ranks due objects by how many of their update periods they
have waited. It charges whole state envelopes before batching. The last
indivisible object can exceed the allowance and its excess is deducted from
subsequent passes. Construction, removal, grants, requested owner corrections
and repair traffic progress independently. ``GetStatistics(observer).deferredState``
counts eligible candidates held back by the budget on the last serialization
pass. The streaming benchmark reports delivered state age in server ticks
for the frequent and infrequent groups.

Interpolation and driver prediction
-----------------------------------

Use ``SnapshotReplica<VehicleState, 16>`` on both peers to keep the latest
16 authoritative samples. Choose matching type IDs, codec versions and
history-enabled codecs on both peers. In a fixed-step game, select manual
tick mode, set the serialization interval to zero, and call
``replication.Tick(simulationTick)`` after each simulation step. It returns
false for a zero, repeated or decreasing tick. The parameterless tick advances
the counter by one. Automatic mode advances it as the receive loop drives
replication.

On the spectator, ask the replication manager for a fractional render tick
and pass it to the vehicle's interpolator. During warmup, display its current
state. All objects from the same source share the clock.

.. code-block:: cpp

   auto tick = replication.GetRenderTick(3);
   auto pose = tick ? vehicle.SampleAt(*tick, interpolate) : vehicle.state;

A client defaults to its server. In peer mode, specify the source with
``GetRenderTick(sourceGuid, 3)``. The delay is in simulation ticks (default
two). Choose at least the update interval of the objects you want to blend.
The clock anchors ticks at local arrival, so delivery latency is already
included. It estimates the simulation rate and adds a buffer based on
observed jitter. The cursor advances between arrivals and never moves
backward for the same delay within a session. After a prolonged gap it holds
its position until fresh samples arrive. ``SampleAt`` clamps at the ends of
each object's history.

Servers and symmetric peers with a history-enabled type send at most one
unreliable clock sample per connection every 100 ms while ticking, including
when every replica is idle. At least two samples are needed. Disconnect and
reconnect start a fresh clock. ``GetStatistics().clockBytes`` counts outgoing
replication clock payload bytes. Sample on the same thread as snapshot
delivery or synchronize reads of the object's state and history.

For a teleport, change the canonical state and call ``MarkTeleport()`` on
the server. Explicitly tracked types also need ``MarkDirty(vehicle)``.
The next received sample snaps to the new position even if the first
teleport packet was lost. A fresh scope visit starts a new history.

For a locally controlled vehicle, keep a separate predicted state and an
``OwnerInputHistory<DriverInput>`` per local copy. Its default capacity is
64 commands. On successful ``inputs.Send(replication, vehicle, command)``,
apply that command immediately to predicted state. A full history or a
refused send returns false so the game can avoid predicting unsent input.
These tracked reports opt that grant into reliable, coalesced owner
corrections. The unchanged public two-argument ``SendOwnerUpdate`` continues
to submit ordinary reports. The server's report handler decides which inputs
affect canonical state. Once processed, even a rejected command retires from
prediction. Other observers receive ordinary snapshots at their own cadence.

In the existing replica callback, resolve ``event.id`` and handle
``ReplicaEventKind::InputProcessed`` with
``predicted = inputs.Reconcile(event.input, vehicle.state, Simulate)``.
The helper retires commands through the server's sequence in that grant and
replays the rest using your simulation function. Clear the history and seed
prediction from canonical state on authority grant, loss or forced correction.
New submissions automatically discard entries from an earlier epoch.

Build and run the ``Replication`` sample for a scripted loopback drive.
Pass ``--drive`` for interactive acceleration, braking, teleport and driver
handoff. The sample configures debug transport latency and jitter. The
deterministic ``ReplicationMotion`` tests also run delayed links in Release.

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
