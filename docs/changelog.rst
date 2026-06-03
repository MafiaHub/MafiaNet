Changelog
=========

All notable changes to MafiaNet are documented here.

Version 0.8.0
-------------

**Core**

* **Optional disconnect reason on graceful disconnects.** ``CloseConnection``
  gains a final optional ``const BitStream *reasonData`` argument whose bytes are
  appended right after the ``ID_DISCONNECTION_NOTIFICATION`` message ID, so the
  remote peer can learn *why* it was dropped (e.g. a kick/ban enum plus a custom
  string). The receiver reads it exactly like any other message body —
  ``packet->data + 1`` for ``packet->length - 1`` bytes. Only graceful
  disconnects carry a reason; locally-synthesized notifications
  (``ID_CONNECTION_LOST`` and the timeout/dead-connection path) stay
  payload-less, so consumers must tolerate a zero-length body. Appending bytes
  after the 1-byte ID is wire-backward-compatible: peers that only inspect
  ``data[0]`` are unaffected.

**Bug fix**

* ``RakPeer::CloseConnection`` no longer coerces an unresolved target index
  (``-1`` from ``GetIndexFromSystemAddress``) to ``0`` and then reads
  ``remoteSystemList[0]`` — which targeted an unrelated peer's slot or crashed
  when the list was unallocated. The close socket is now resolved without
  assuming a valid slot index.

**Documentation**

* Added a "Disconnect with a reason" section to the connecting guide and a
  cross-reference from the disconnect-debugging guide.

**Testing**

* Added ``DisconnectReasonTest`` covering reason round-trip, the ``nullptr``
  default, and the empty-but-non-null ``BitStream`` guard.

Version 0.7.0
-------------

**Plugins**

* **Virtual worlds (dimensions) for ReplicaManager3.** A new lightweight
  per-entity / per-observer ``VirtualWorldId`` tag scopes visibility at runtime —
  the SA-MP ``SetPlayerVirtualWorld`` / routing-bucket model for instanced
  interiors such as apartments. Players only see entities sharing their virtual
  world (or the ``VIRTUAL_WORLD_GLOBAL`` sentinel), switchable on the fly with no
  reconnect, while staying on the same connection and the same RM3 ``WorldId``.
  Derive entities from the new ``VirtualWorldReplica3`` base
  (``mafianet/VirtualWorldReplica3.h``); ``Connection_RM3`` gains
  ``Get/SetVirtualWorld``; ``ReplicaManager3`` gains
  ``GetConnectionsInVirtualWorld`` / ``GetGuidsInVirtualWorld`` (recipient-filter
  helpers for scoping non-replica traffic like chat and RPC) and
  ``SetPlayerVirtualWorld``. The filter is applied only by the authority for an
  (entity, connection) pair, so a downloaded copy never despawns the entity at
  its owner. See ``mafianet/VirtualWorld.h`` and the ``Samples/VirtualWorld`` demo.

**Documentation**

* Added a "Virtual Worlds (Dimensions)" plugin guide and expanded the
  contributing guide with how to test networked features (unit + end-to-end), the
  ReplicaManager3 authority model, and multi-peer-in-one-process gotchas.

**Testing**

* Added ``VirtualWorldTest`` (deterministic unit coverage, including the
  non-authority case) and a self-contained ``Samples/VirtualWorld`` smoke test.
* Fixed the test harness so a subset run (``Tests <name>``) calls
  ``DestroyPeers()`` on the test that actually ran.

Version 0.6.1
-------------

**Plugins**

* **ReplicaManager3::GetReplicaAtIndex is now const.** It was the only one of the four read accessors (``GetReplicaCount``, ``GetReplicaAtIndex``, ``GetConnectionCount``, ``GetConnectionAtIndex``) that was non-const, which forced ``const`` methods on derived managers to ``const_cast`` away constness just to iterate replicas. The accessor only reads the world's replica list and returns an existing pointer, so the qualifier is accurate; the returned ``Replica3*`` stays non-const, matching ``GetConnectionAtIndex``. Source-compatible — adding ``const`` to a read accessor doesn't break existing non-const call sites.

Version 0.6.0
-------------

**Plugins**

* **RPC4 handlers now carry user context.** ``RegisterFunction``, ``RegisterSlot``, ``RegisterBlockingFunction`` and the ``RPC4GlobalRegistration`` handler constructors take an opaque ``void *context`` that is passed back to the handler on every invocation. This removes the need for file-static global pointers to route an RPC back to an object instance; each registration carries its own context, so the same handler may serve multiple object instances under one identifier. The ``void*`` approach preserves RPC4's zero-external-dependency design.

**Bug Fixes**

* ``RakPeer::CloseConnection`` no longer dereferences a null ``rakNetSocket`` during connection teardown (a pre-existing crash in release builds, where the assertion is compiled out); it now falls back to the primary socket, matching the existing buffered-close path.

**Testing**

* Added ``RPC4ContextTest`` covering slot, nonblocking, and blocking handler context.
* Quarantined the flaky ``ManyClientsOneServerDeallocateBlockingTest`` under CI while a pre-existing multithreaded teardown race is investigated.

**Breaking Changes**

* RPC4 handler signatures gained a trailing ``void *context`` parameter, and the registration / global-registration functions take a context argument. There are no compatibility overloads — update handlers and registration calls (pass ``nullptr`` when no context is needed).

Version 0.5.1
-------------

**Plugins**

* Added ``DirectoryDeltaTransfer::AddFile(const char *filePath, const char *fileName)`` to queue a single file for upload, complementing the recursive ``AddUploadsFromSubdirectory``. It forwards to the existing ``FileList::AddFile`` overload, making the fork self-sufficient for downstream consumers (MafiaHub Framework) that depend on this helper.

Version 0.5.0
-------------

**API & Namespace Cleanup**

* Standardized on the ``MafiaNet`` namespace throughout the library
* Removed the legacy ``SLNet`` compatibility macro and replaced it with an ``MNet`` short-hand alias that expands to ``MafiaNet``
* Dropped stale ``RakNet`` namespace-alias references (the alias no longer existed in code) and migrated the remaining sample code to ``MafiaNet::``
* Collapsed the three header layers inherited from the RakNet/SLikeNet lineage down to the single canonical ``Source/include/mafianet/`` set; the redirect-only ``Source/*.h`` and ``Source/mafianet/*.h`` stubs were removed and all includes now use the ``mafianet/...`` form

**Bug Fixes**

* Guarded ``BitStream``'s catch-all ``Write``/``Read`` templates with a ``std::is_trivially_copyable`` ``static_assert``, preventing silent pointer-aliasing and double-frees when serializing types that own heap memory (e.g. ``std::string``)
* Added binary-safe, length-prefixed ``std::string`` ``BitStream`` specializations (wire-compatible with ``RakString``)
* Fixed ``Ranking_GetMatches`` serializing ``SubmittedMatch`` through the unsafe catch-all instead of its own ``Serialize()``

**Testing**

* Added the ``BitStreamStringTest`` regression test

**Breaking Changes**

* Legacy include spellings are gone — include public headers via the ``mafianet/...`` path (e.g. ``mafianet/string.h`` instead of ``RakString.h``)
* The ``SLNet`` namespace macro has been removed — use ``MafiaNet`` (or the new ``MNet`` shorthand)
* Serializing a non-trivially-copyable type through the generic ``BitStream::Write``/``Read`` now fails to compile by design; provide an explicit ``Serialize()`` or a type specialization

Version 0.4.0
-------------

**Cross-Platform Support**

* Full macOS and Linux compatibility, including merged ``Socket2`` definitions
* Removed deprecated platform back-ends to simplify the socket layer
* Fixed IPv6 connectivity and initialization issues
* Guarded the ``<sys/io.h>`` include to x86/x86_64 only (fixes ARM builds)

**Dependencies**

* Build pipeline upgraded to OpenSSL 3.6.0 (3.0+ still required)
* Updated miniupnpc 2.2.8 → 2.3.3
* Updated Opus 1.5.2 → 1.6.1
* Dependencies are now fetched on demand via CMake ``FetchContent`` instead of being bundled in-tree

**Bug Fixes**

* Fixed undefined behaviour from a negative ``double`` → ``unsigned`` cast in congestion control
* Guard against a null socket in ``BCS_CLOSE_CONNECTION`` handling
* Fixed DLL exports on Windows
* Fixed sample compilation across all platforms

**Testing & CI**

* CI now builds and runs the full test suite on Linux, macOS and Windows (stress tests included)
* Added a Dockerfile for running the test suite in a container
* Numerous test-stability improvements: mesh-convergence waits, race-condition fixes, and a thread-safe plugin lifecycle for ``PacketChangerPlugin``

Version 0.3.0
-------------

**RakVoice: Speex to Opus Migration**

* Replaced deprecated Speex codec with Opus 1.5.2
* Added RNNoise for neural network-based noise suppression
* Supported sample rates changed to native Opus rates: 8000, 16000, 24000, 48000 Hz
* VAD now uses Opus DTX (Discontinuous Transmission)
* Added ``SetSignalType()`` for voice/music optimization hints
* Removed ``SetEncoderComplexity()`` / ``GetEncoderComplexity()`` (Opus handles internally)
* Bundled Opus and RNNoise sources (no external dependencies)
* Removed Speex and SpeexDSP from DependentExtensions

**Breaking Changes:**

* Sample rate 32000 Hz is no longer supported (use 24000 or 48000)
* ``SendFrame()`` and ``RequestVoiceChannel()`` now use ``RakNetGUID`` instead of ``SystemAddress``

Version 0.2.0
-------------

* Rebranded from SLikeNet to MafiaNet
* Updated to C++17 standard
* Modernized CMake build system
* Added Sphinx documentation with Breathe integration
* Removed pre-generated Visual Studio solution files
* Updated dependencies (miniupnpc, OpenSSL)

Version 0.1.0
-------------

* Initial fork from SLikeNet
* Basic project structure established
