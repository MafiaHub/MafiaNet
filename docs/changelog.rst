Changelog
=========

All notable changes to MafiaNet are documented here.

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
