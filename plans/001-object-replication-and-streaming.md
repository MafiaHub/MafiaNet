# Built-in object replication and spatial streaming

**Design proposal · MafiaNet `2ba07f5d` · 22 November 2026**

Every UDP peer owns one C++17 replication service, accessible as `peer->GetReplication()` before or after startup, just like `peer->GetRPC()`. The peer drives the service directly. A game registers a replica class, references a game object, and supplies state through familiar RM3 callbacks. A server can place that object in a spatial grid, admit observers, and grant a particular client permission to report selected state. All three facilities use the same network identity and object lifecycle. The public API should work for a game with ordinary C++ classes and for an engine that already owns its entities.

## Implemented release

MafiaNet 0.27.0 assigns a single message ID to built-in replication on transport protocol 12. Both endpoints register stable game type IDs and codec versions. A session has one object ID namespace. Worlds and dimensions select the server-side audience. Each owner-report format is identified by the replica type and its registered report version. A lost final replaceable snapshot is sent reliably after 250 ms without application acknowledgment.

The [public guide](../docs/guide/replication.rst) and [two-client sample](../Samples/Replication/main.cpp) use the implemented API. The sample runs on loopback. The deterministic acceptance suite is in [BuiltinReplicationTests.cpp](../Tests/Unit/BuiltinReplicationTests.cpp).

On Apple M5 Pro with Apple Clang 21 and the Release bench preset, the grid workload with 50,000 objects and 20 observers inspected zero candidates per tick at rest and after unrelated distant motion. The clustered move rebuilt four observer sets. Teleporting every observer rebuilt twenty. These counters describe spatial queries. The serializer and game callbacks do additional work.

## Design inputs

| Evidence | Decision for MafiaNet |
|---|---|
| Replicon has explicit replication rules, per-client visibility, admission and a configurable server tick. Its visibility API gives games a place to define relevance. | Register replica **types**, make observer admission explicit when needed, and ship a ready-to-use spatial policy. Games should be able to select a radius or a set of cells without writing a visibility engine. [Replicon 0.45.6 guide](https://docs.rs/bevy_replicon/0.45.6/bevy_replicon/#client-visibility) |
| Replicon orders structural changes and handles replaceable mutations with acknowledgments and dependencies. | A new observer needs a baseline before applying subsequent state. Keep ordinary RM3 reliable delivery as the starting point. Specify convergence before offering a built-in lossy state mode. [Replicon channels](https://docs.rs/bevy_replicon/0.45.6/bevy_replicon/shared/backend/channels/enum.ServerChannel.html) |
| Bevy can observe mutable component access. Its `Changed<T>` filter can still visit every matching entity. | Plain C++ objects use existing RM3 comparison by default. Offer explicit dirty notification to games that already know what changed. Measure work as well as bandwidth. [Bevy change detection](https://docs.rs/bevy/latest/bevy/prelude/struct.Changed.html) |
| Framework `1d2ba321` implements range hysteresis, budgets and owner handling in `code/framework/src/networking/replication/interest_grid.h`. Its `replication_manager.cpp` separately grants simulation, handles disconnect and decides what to destroy. | Use these as downstream cases. Put spatial membership and authority transfer in MafiaNet. Keep NPC elections, engine coordinates, player-avatar conventions and gameplay validation in the game. Framework pins MafiaNet 0.26.0. |

## Public API and ownership

Add `ReplicationManager& GetReplication()` to `RakPeerInterface` and implement it in `RakPeer`. The owning `Peer` handle exposes it through its existing `operator->`. Follow the PascalCase convention of `GetRPC()`, `Reference()` and `Dereference()`. Use `PeerGuid` for peers, `NetworkID` for objects and a strong `ReplicaTypeId` for game-defined wire type IDs. Make the service peer-constructed and noncopyable. Its public type is independent of `PluginInterface2`.

```cpp
// Proposed API. Both endpoints register the same codecs before admission.
auto& replication = peer->GetReplication();
replication.SetRole(role);  // Authority is an application decision on a UDP peer.
replication.RegisterReplica<Vehicle>(VehicleType, VehicleCodecVersion);
replication.RegisterOwnerUpdate<Vehicle, DriverPose>(1, &Vehicle::AcceptDriverPose);

// Server setup. Receive-driven updates are the default.
replication.SetTickMode(MafiaNet::ReplicationTickMode::Manual);
auto& interest = replication.GetInterestGrid();
if (!interest.Configure(cityGrid))  // bounds and cell size in game units
    return StartupFailed;
interest.SetObserverRadius(driverGuid, driverPosition, enterRadius, leaveRadius);

// A positioned Reference enrolls the replica and its initial scope together.
replication.Reference(&vehicle, MafiaNet::SpatialScope{vehiclePosition});
replication.SetAuthority(vehicle, driverGuid);

// Simulation loop, after accepting inputs and updating positions.
interest.SetPosition(vehicle.GetNetworkID(), vehiclePosition);
interest.SetObserverPosition(driverGuid, driverPosition);
replication.Tick();

// On the controlling client during its own simulation.
if (replication.HasLocalAuthority(vehicle))
    replication.SendOwnerUpdate(vehicle, driverPose);
```

The example illustrates shape. Production examples check registration, authority and startup results. The reference returned by `GetReplication()` stays valid until peer destruction. Reject duplicates, unsupported roles and incompatible peers with inspectable results in the style of the peer APIs. Stage 2 should compile a tiny client and server against the proposed headers before expanding them.

The quick start should be smaller than the vehicle example. A door with one boolean can use the snapshot helper, register the type on each peer and call `Reference(&door)`. A normal `door.Open()` changes the next reliable snapshot. The client receives the open state when it joins. No interest configuration, dirty marking or authority grant is needed for that path. Present the vehicle and grid as the next example.

- **Local object.** `Reference` borrows it. Final `Dereference` ends local tracking and waits for in-flight access according to the existing threading contract. Explicit broadcast destruction tells observers the object is gone. Provide one convenience operation combining broadcast and dereference for the common case. It leaves deletion to the local owner.
- **Remote object.** A default factory constructs the registered type. A factory/release overload supports pools and engine-managed objects. Exactly one release accompanies each successful remote allocation, including rejection after allocation, scope loss, disconnect and shutdown. Snapshot decoding succeeds before the game sees a ready replica. A stable network ID may identify a new local representation after re-entry. Document lookup and callback lifetimes.
- **Serialization.** Offer a small optional registered-replica base that supplies type ID, topology defaults and default release routing. Keep `SerializeConstruction`, `DeserializeConstruction`, `Serialize` and `Deserialize` as customization points. Include a convenience codec for a single checked full-state snapshot used for initial construction and reliable changes. Reuse `BitStream` and the archive field convention. The existing `ReadArchive` lacks a success result. Provide a checked decoder or require a status-returning codec for the convenience path.
- **Time and threads.** Automatic mode uses the ordinary receive loop. In manual mode, receiving still processes connections and packets, while only `Tick()` starts a construction/serialization pass. Sample on a single game thread first. Document which callbacks can still run on the thread calling `Receive()`, how a game queues its own notifications, and how tick work interacts with `WorldGuard`, `StateDelivery` and the existing executor. A manual tick whose game callback calls `Receive()` must run once.

## Peer lifecycle and migration

| Peer event | Replication behavior |
|---|---|
| Construction and startup | Each peer has its own service and type registry. Allocate spatial cells and per-connection state on demand. An application using only transport emits no replication traffic and performs no grid scans. |
| Connection and receive | Feed connection opens and closes directly from the peer lifecycle. In automatic mode the peer drives one replication update at the established receive-loop update point. Route eligible replication packets after message filters and respect the peer shutdown-generation check around callbacks. Internal control records are consumed exactly once. |
| Shutdown and restart | While the peer can still release packets and IDs, drain in-flight work, release downloaded objects, unbind borrowed replicas, and clear connection membership, grants, grid entries and pending work. Preserve type registrations, role, and grid configuration for reuse. Reopen admission with a fresh session identity on restart. Destroy the service and registry with the peer. |
| Existing RM3 applications | Migrate setup to `GetReplication()`. Move custom connection creation, allocation/release and codecs onto service-owned factories or policies. Reuse replica subclasses, serialization callbacks and register their current codecs. Port the low-level and virtual-world samples in the same release. Give downstream users a mapping for each former manager override, including custom scheduling and packet handling. |
| Build and ABI | Compile replication into standard and minimal static and shared libraries. Retire `ReplicaManager3` from the optional-feature list and its compile guards. Test the installed package. Adding the peer interface method changes the ABI. Update peer implementations and test doubles, and require downstream binaries to rebuild. |

## Spatial streaming contract

1. **Two common modes.** A radius observer supplies a 2D position, enter radius and leave radius, with `leave >= enter >= 0`. A cell subscriber supplies an explicit set of integer cells (for rooms, tiles or chunks). Expose a replacement operation such as `SetObserverCells(guid, cells)` that applies the complete set in one tick. Start with one observer per connection per RM3 world. Moving the observer independently of its avatar supports cameras and spectators. Supply 2D coordinates directly so XY and XZ games use the same interface.
2. **Eligibility order.** Check admitted connection and RM3 `WorldId`, then the existing `VirtualWorldId` dimension rule and any game audience restriction. Then evaluate global-within-audience objects, required authority baselines, and ordinary spatial membership. A driver can stay in scope outside the normal radius while eligible. Loss of admission, dimension access or explicit permission revokes the grant before streaming out. Explicitly distinguish a globally *spatially* visible replica from the `VIRTUAL_WORLD_GLOBAL` dimension tag. A basic non-spatial `Reference` uses ordinary RM3 visibility. An observer with no position or cell subscription sees only eligible non-spatial objects and explicit authorized inclusions.
3. **Index and exact membership.** Use bounded 2D uniform cells for v1. Use `PointGridSectorizer` for broad-phase radius candidates. It returns a cell superset, clamps coordinates, stores pointer keys and has no thread synchronization. Provide stable manager-owned spatial records, validate finite positions and bounds at the public boundary, and specify a uniform outside-bounds policy (proposed: reject and retain the last accepted position). Use floor-based half-open cells from the configured origin, with the outer maximum included in the last cell. Provide checked point-to-cell conversion and cell enumeration shared by radius and explicit-cell APIs. Use actual squared distance for the radius decision. Include at the enter boundary and retain through the leave boundary.
4. **Movement and invalidation.** Track subscribed cells back to their observers. A radius observer subscribes to the broad-phase rectangle covering its leave radius. Moving an entity dirties candidate observers covering the old or new location and any existing holders. Moving an observer recomputes its own candidates and diffs against its previous set. Exact positions matter even when both points stay in the same cell. For explicit subscriptions only a change of cell, membership, eligibility or budget can change the set. Handle teleports, subscription replacement, dimension changes, reassigned authority, deletion, reconnect and grid reconfiguration. Coalesce writes and evaluate one consistent view of the world per tick. Use stable IDs/generations for queued work and reclaim records after the pass releases them.
5. **From membership to packets.** The managed spatial connection uses RM3 connection-list mode. `QueryReplicaList` receives unique **enter/leave deltas**. It bypasses the normal per-object construction query, so enforce audience and dimension rules on this path too. Compare desired membership against RM3's actually constructed set, including deferred entries. Enter sends a reliable current construction snapshot. Leave releases only that observer's remote copy. Re-entry seeds current state even if no dirty event occurred. Actual destruction cancels pending entries and removes every index entry. For the managed protocol, distinguish scope loss from permanent deletion when they are observable. Existing static, already-present level objects keep their specialized RM3 path until a dedicated static-binding test proves equivalent behavior.
6. **Bounded work.** Offer an optional visible-object cap per observer and a per-tick construction budget to smooth teleports and late joins. Apply hard eligibility removals immediately. Required authority baselines take priority. Rank ordinary eligible entries by explicit game priority, squared distance and stable ID. Provide a documented retention margin to prevent churn near the cap. Age deferred eligible construction so a busy stream proceeds fairly. Re-check eligibility immediately before sending. Record desired versus constructed counts, queued entries, cells visited, candidates inspected, enters/leaves and bytes by message kind. Expose aggregate counters and an opt-in per-observer view.

For example, with 64-unit cells, an observer at `(63, 20)`, a 10-unit enter radius and a 15-unit leave radius, a vehicle at `(72, 20)` enters across the cell boundary. At `(77, 20)` it stays visible. At `(79, 20)` it leaves, even though all three vehicle positions lie in the same cell. A subscriber to cell `(1, 0)` keeps the vehicle throughout those moves. Use this as the smallest executable example of the two streaming modes.

Dense bounded grids fit many towns and arenas. Measure memory before selecting default bounds or allocating a full grid per dimension. Start with one index per configured RM3 world and filter dimension tags on candidates. The stable cell API leaves room for a sparse or 3D index if large-world measurements justify one.

## Authority, protocol and delivery

- **Object authority.** The server owns the canonical ID and lifetime by default. A game may give one admitted client permission to submit a typed report on an instance. The server hook validates the decoded value and applies, clamps or rejects it. The ordinary downstream serializer publishes accepted state and server-owned fields. Clients can request creation or destruction through existing messages/RPC. The game approves and references or removes the object.
- **Grant lifecycle.** Send a reliable grant/revoke with session, object identity and generation. Construct or refresh the new driver's baseline before enabling its writes. A correction that supersedes in-flight owner input advances the generation and supplies a new baseline. Verify connection identity and generation before application callbacks, independent of whether the bytes arrived singly or through a custom decoder using the managed API. Apply a per-report sequence for replaceable samples within a grant. Define serial-number comparison, wrap and reconnect behavior. A disconnect revokes the grant and retains canonical state by default. Notify the game so it can despawn an avatar or select a replacement. Do not key present-day rights by `creatingSystemGUID`.
- **Managed wire profile.** Freeze explicit type IDs, codec versions, owner-report IDs and feature bits before accepting replication traffic. Compare a canonical sorted manifest fingerprint after application admission and before remote construction. Application versioning covers codec semantics. Put control records in a versioned extension with bounded lengths, checked decode, sender validation and predictable unknown-message behavior. Assign current message names directly and advance the transport protocol version. On mismatch return a replication-specific reason and withhold enrollment. Both peers use one replication protocol.
- **Delivery modes.** Ship reliable ordered lifecycle and reliable state as the baseline. Existing per-channel `PRO` and custom serialization remain available. For a built-in replaceable-state mode, include scope incarnation, sequence and a baseline dependency. Drop old-incarnation updates, coalesce unsent values and refresh the latest unconfirmed state until delivery is confirmed or newer state supersedes it. Test loss of the final movement packet followed by silence. An old pose from a previous visit to a cell must not move a fresh copy. Keep implicit byte comparison as the default for ordinary replicas. Add explicit `MarkDirty` as a type-level opt-in with per-recipient pending work, snapshot on entry, and a bulk-change fallback. Measure the existing serialization-pass scan separately from the spatial broad phase.

## Implementation sequence

All stages are proposed. Effort is relative engineering size, not a delivery date. Each stage includes its own regression tests.

| PR | Deliverable and principal files | Depends on | Size / risk | Acceptance |
|---|---|---|---|---|
| 1 | Repair RM3 lifetime and world invariants in `Source/src/replica_manager3.cpp` and its header. Initialize reference ordering, clear the final manager back-pointer, define supported multi-world membership, propagate world IDs through connection creation/destruction, and clean up populated worlds. Cover shutdown, re-reference, custom release, and payload validation on touched paths. | Existing baseline | M / high | ASan lifetime tests, world 0 and 255 isolation, threaded delivery teardown and existing tests pass. A borrowed entity can outlive the peer after final dereference. |
| 2 | Land `GetReplication()` in `peer_interface.h` and `peer.h` and wire the owned service directly into `peer.cpp` startup, receive, update, close and shutdown. Add `replication_manager.h/.cpp`, the connection factory, per-peer registry, default ID manager, snapshot helper, tick mode and admission. Move the feature into the core build. Add the managed protocol handshake before exposing managed registration to consumers. Share session teardown with RM3. | 1 | L / high | Two simulated peers replicate a door through their built-in services. Each peer returns the same service across restart, retains its own registrations, and cleans up session state. Duplicate IDs, unknown types, malformed snapshots, mismatched versions, restart and pooled objects have deterministic outcomes. Compile the public API against static and shared libraries. |
| 3 | Implement `interest_grid.h/.cpp` as a typed policy and extend `point_grid_sectorizer` only for needed cell operations. Implement exact radius, hysteresis, explicit cells, reverse subscriptions and deterministic policy tests. | Can develop alongside 1–2 | M / medium | Fixed-seed differential tests against brute force compare every observer after each mutation. Include same-cell crossings, negative coordinates, exact edges, empty cells, teleports, opposing moves, invalid input and slot reuse. |
| 4 | Integrate interest with the concrete connection's construction list, virtual worlds, visibility events and construction budget. Snapshot once per tick under RM3's lifetime rules. Preserve synchronous automatic mode and manual mode. | 2, 3 | L / high | Simulated late join, movement, scope exit/re-entry, multi-world isolation and pending-spawn cancellation under load. Pumping `Receive()` repeatedly in manual mode never advances the stream. A hidden observer receives no subsequent state. |
| 5 | Implement authority grants, typed owner updates and correction through the managed connection in `replication_manager.cpp`. Keep validation and lifetime policy separate from interest ranking. | 2, 4 | L / high | Three-peer transfer Alice → Bob → Alice under delay/reordering. Old grants fail, eligible observers get accepted state, server-only fields reach the driver, and disconnect releases authority. Granting to an authorized observer outside radius and revoking after dimension change both work. |
| 6 | Add opt-in dirty scheduling, replaceable delivery and telemetry. Extend `Benchmarks/replica_manager_bench.cpp` and the existing simulated dirty-list and threaded suites. | 4, 5 for combined acceptance | L / high | Final lost pose converges, prior-incarnation state is ignored, per-observer deferrals survive dirty-list rotation, and worker/serial results agree. Benchmark sparse, clustered, bulk and churn cases against the baseline. |
| 7 | Publish a compiled `Samples/Replication` example, architecture guide, quick start and migration table. Replace invalid RM3 tutorial setup with code drawn from the compiled example. Add installed-package coverage to `ci/consumer`, minimal-build checks, documentation links and release notes. | 2–6 | M / low | One sample covers a server, two observers, a vehicle handoff and a room transition. New users follow it through the ordinary peer receive loop. Migrated low-level and virtual-world samples and the minimal installed consumer all use the built-in service. |

**First usable milestone:** PRs 1–4 deliver ordinary replication and spatial streaming. PRs 5–7 complete the proposed first-class release. Open the small API/sample portion of PR 2 for maintainer review before freezing headers or wire allocations. Prediction, rollback, presentation interpolation, automated simulator elections, sparse 3D worlds and engine-specific adapters are follow-on proposals with separate workloads.

## Verification and release gates

Run from the MafiaNet root. Proposed suite names in the focused command become part of the implementation. Record baseline commit, compiler, flags, machine and benchmark arguments for comparisons.

```sh
cmake --workflow --preset debug
ctest --test-dir build/debug -R 'Replication|ReplicaManager3|Rm3ReceiveThread|PointGridSectorizer|InterestGrid|VirtualWorld' --output-on-failure
cmake --workflow --preset asan
cmake --workflow --preset tsan
cmake --preset minimal
cmake --build --preset minimal
cmake --preset bench
cmake --build --preset bench
./build/bench/Benchmarks/MafiaNetBenchmarks --benchmark_filter='ReplicaManager3|InterestGrid'
ci/format.sh --check
```

- **Behavior.** Drive the release scenario through real public entry points on `Tests/Support/SimulatedNetwork.h`. Two clients enter and leave radius and explicit-cell scopes, one joins late, both drive the same car in succession, then one disconnects. Check the exact visible ID sets, current snapshot bytes/values, single construction and release per visit, authority state and continued delivery to the other client. Include a transport-only peer, mismatched schema, manual enrollment and back-to-back shutdown/startup.
- **Concurrency and decoding.** Use the existing `Rm3ReceiveThread` fixture for true receive/tick overlap, removal while processing state, registration lifetime and restart. Exercise callbacks that queue another mutation or release an observer. Run the debug, ASan and TSan suites on supported CI platforms. Add codec corpus replay for the managed envelope and snapshot framing under `Tests/Fuzz`. Reject incomplete data before committing game state. Run the full suite for peer lifetime or routing changes.
- **Performance.** Reuse the 50,000-replica / 20-observer workloads. Add stationary observers with distant unrelated replicas, movement entirely within cells, crowded-cell crossings, mass teleport and churn with per-tick caps. Report CPU distribution, allocations, memory, candidate counts, membership churn, pending age and bytes. An idle tick after warmup should rebuild zero observer sets and produce zero lifecycle messages. Adding distant replicas must not raise local candidate counts. Set numeric timing budgets from repeatable baseline runs before optimizing. Keep wall-clock benchmarks manual per repository convention.
- **Distribution.** Exercise the installed `GetReplication()` API in standard and minimal builds. Use existing install-consumer CI for Linux, macOS and Windows, shared and static. Account for ABI changes independently of protocol compatibility. Verify both ends agree on their replication schema before admitting replicas. Test application filter ordering and packet visibility.
- **Maintenance.** Keep grid coordinates, authority rights and codec IDs in separate types. Additions to the grid API should have a brute-force oracle case. Each new wire record needs checked length, version behavior, and a mixed-version test. Record the chosen envelope allocation and compatibility policy in the release notes.

## Repository entry points

| File | Why it matters |
|---|---|
| `Source/include/mafianet/peer_interface.h`, `peer.h` and `Source/src/peer.cpp` | Follow the peer-owned RPC accessor (interface line 52), member ownership and post-filter receive dispatch. Cover shutdown from inside a game callback. |
| `Source/include/mafianet/replica_manager3.h` | Public callback contracts, threading rules, connection-list mode at line 716 and serialization candidates at line 736. |
| `Source/src/replica_manager3.cpp` | Final dereference around line 622, list-mode streaming in `AutoConstructByQuery`, and creator-based client serialization around line 3392. |
| `Source/include/mafianet/point_grid_sectorizer.h` | Existing spatial index. Same-cell moves can affect precise visibility even when `MoveEntry` returns false. |
| `Source/include/mafianet/virtual_world_replica3.h` and `virtual_world.h` | Existing authority-side dimension behavior and global sentinel semantics. |
| `Tests/Unit/ReplicaManager3DirtyListTests.cpp`, `ReplicaManager3ListModeTests.cpp`, `PointGridSectorizerTests.cpp` | Reusable patterns for late join, scope re-entry, released serialization records and differential tests. |
| `Tests/Integration/Rm3ReceiveThreadLiveTests.cpp`, `Benchmarks/replica_manager_bench.cpp` | Existing concurrency contract and performance comparisons. |
| `Source/CMakeLists.txt`, `ci/consumer`, `docs/guide/replication.rst` | Installed/public feature surface and onboarding. |
