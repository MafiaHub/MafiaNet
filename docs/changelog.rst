Changelog
=========

All notable changes to MafiaNet are documented here.

Unreleased
----------

.. rubric:: Breaking changes

* **Mandatory default encryption (wire-incompatible with 0.9.x and earlier).**
  Every connection is now encrypted and authenticated using the
  ``Noise_NK_25519_ChaChaPoly_SHA512`` cipher suite (libsodium).  There is no
  opt-out.  Peers built against this version cannot talk to peers from any
  earlier release; plan a coordinated server + client upgrade.

* **``Connect`` / ``ConnectWithSocket`` signature changed.** The former
  optional ``PublicKey*`` fifth argument is replaced by a *required*
  ``const unsigned char serverPublicKey[32]`` — the server's pinned 32-byte
  X25519 public key.  Callers that omitted the argument (plain unencrypted
  connections) must now supply the key; there is no unencrypted fallback.

* **Removed API:**

  - ``RakPeerInterface::InitializeSecurity`` — replaced by
    ``RakPeerInterface::SetServerSecurityKey(const ServerSecurityKey&)``.
  - ``RakPeerInterface::DisableSecurity`` — encryption cannot be disabled.
  - ``RakPeerInterface::AddToSecurityExceptionList`` /
    ``RemoveFromSecurityExceptionList`` — exception list concept removed.
  - ``RakPeerInterface::GetClientPublicKeyFromSystemAddress`` — client is
    anonymous; no client key is exchanged.
  - ``PublicKey`` struct and ``PublicKeyMode`` enum (from
    ``mafianet/types.h``).
  - ``GenerateServerRSAKeys`` — replaced by
    ``MafiaNet::GenerateServerSecurityKey()`` (returns ``ServerSecurityKey``).
  - libcat / ``SecureHandshake.*`` / ``LIBCAT_SECURITY`` preprocessor flag.

* **``FullyConnectedMesh2::SetConnectOnNewRemoteConnection``** gained an
  optional third parameter ``const unsigned char serverPublicKey[32] = 0``.
  Auto-mesh connections require a shared pinned key; without it the call is
  fail-closed (no auto-connections are made).

.. rubric:: New features

* **``MafiaNet::ServerSecurityKey``** (``mafianet/crypto/keys.h``): plain
  struct holding a 32-byte X25519 ``publicKey`` and ``secretKey``.
* **``MafiaNet::GenerateServerSecurityKey()``**: generates a new X25519
  identity keypair for the server.
* **``RakPeerInterface::SetServerSecurityKey``**: installs the server's
  long-term identity keypair (required before ``Startup``).
* Transport encryption: ChaCha20-Poly1305 AEAD per datagram with a 64-entry
  sliding-window anti-replay filter.
* DoS mitigation: stateless HMAC return-routability cookie gates handshake
  work on the server.

.. rubric:: Dependencies

* **libsodium** is now a required dependency (fetched automatically via CMake
  FetchContent).  OpenSSL is still required for TLS-layer features.
* libcat has been removed.

Version 0.9.0
-------------

**Core**

* **Strong-typed** ``PeerGuid``. A new ``enum class PeerGuid : uint64_t`` names a
  peer's ``RakNetGUID`` value distinctly from ``NetworkID`` (an object id), so the
  two can no longer be passed interchangeably in a ``uint64_t``-typed signature —
  removing a class of silent "passed the wrong id" bugs in ReplicaManager3 glue
  and ``void(uint64_t)`` callbacks. Convert with ``ToPeerGuid()`` / ``ToGuid()``,
  and compare against the ``UNASSIGNED_PEER_GUID`` sentinel. Being a
  trivially-copyable 8-byte scoped enum, it serializes byte-identically through
  ``BitStream`` (and therefore ``VariableDeltaSerializer``) to the raw
  ``uint64_t`` it replaces, so it is fully wire-compatible and requires no
  netcode/protocol bump. Purely additive — no behavioural change.

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
