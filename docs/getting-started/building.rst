Building MafiaNet
=================

Linux / macOS
-------------

.. code-block:: bash

   # Configure
   mkdir build && cd build
   cmake ..

   # Build
   cmake --build .

   # Or for release build
   cmake --build . --config Release

Windows (Visual Studio)
-----------------------

.. code-block:: powershell

   # Generate Visual Studio 2022 solution
   cmake -G "Visual Studio 17 2022" -A x64 -B build

   # Build from command line
   cmake --build build --config Release

   # Or open build/MafiaNet.sln in Visual Studio

Build Options
-------------

Configure build options with CMake:

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``MAFIANET_BUILD_SHARED``
     - ON
     - Build shared library (.dll/.so/.dylib)
   * - ``MAFIANET_BUILD_STATIC``
     - ON
     - Build static library (.lib/.a)
   * - ``MAFIANET_BUILD_SAMPLES``
     - OFF
     - Build sample applications
   * - ``MAFIANET_BUILD_TESTS``
     - OFF
     - Build GoogleTest suite (requires ``MAFIANET_BUILD_STATIC=ON``)
   * - ``MAFIANET_BUILD_EXTRAS``
     - OFF
     - Build ``MafiaNetExtras`` (SMTP, HTTP client, telnet/console transports). Forced ON by ``MAFIANET_BUILD_SAMPLES``. See `The extras library`_.
   * - ``MAFIANET_DISABLED_FEATURES``
     - (empty)
     - Semicolon-separated plugin names to compile out, e.g. ``"Router2;ReadyEvent"``. See `Minimal builds`_.
   * - ``MAFIANET_MINIMAL``
     - OFF
     - Compile out every optional plugin, keeping the built-in RPC and replication services. See `Minimal builds`_.
   * - ``MAFIANET_SANITIZER``
     - (empty)
     - Clang/GCC sanitizers, ``+``-separated: ``address+undefined`` or ``thread``. Applied to every target, dependencies included. See `Sanitizers`_.
   * - ``MAFIANET_USE_COMPILER_CACHE``
     - ON
     - Use ``sccache`` or ``ccache`` as the compiler launcher when one is on ``PATH``. An explicit ``CMAKE_<LANG>_COMPILER_LAUNCHER`` takes precedence.
   * - ``MAFIANET_WARNINGS_AS_ERRORS``
     - OFF
     - Treat warnings in MafiaNet's own code (``-Wall -Wextra``, ``/W4``) as errors. Fetched dependencies are never affected.
   * - ``MAFIANET_BUILD_FUZZERS``
     - OFF
     - Build the libFuzzer harnesses (Clang only). See `Fuzzing`_.
   * - ``MAFIANET_BUILD_BENCHMARKS``
     - OFF
     - Build the google/benchmark microbenchmarks. See `Benchmarks`_.
   * - ``MAFIANET_BUILD_SOAK_TESTS``
     - OFF
     - Build the soak tests (CTest label ``soak``; needs ``MAFIANET_BUILD_TESTS``). See `Soak tests`_.
   * - ``MAFIANET_COVERAGE``
     - OFF
     - Instrument for line coverage (GCC/Clang). See `Coverage`_.

Example with options:

.. code-block:: bash

   cmake -DMAFIANET_BUILD_SAMPLES=ON -DMAFIANET_BUILD_TESTS=ON ..

Minimal builds
--------------

Every optional plugin in the core library (``FileListTransfer``, the NAT traversal plugins, ``TCPInterface`` and its
users, and so on) is guarded by a ``_RAKNET_SUPPORT_<Name>`` flag declared in
``mafianet/native_feature_includes.h``. The transport itself, ``BitStream``,
``RakPeer``, the reliability layer, ``RPC``, object replication and ``RakVoice`` are always built.

The supported way to compile plugins out is through CMake, not by editing the
header:

.. code-block:: bash

   # Drop specific plugins
   cmake -B build -DMAFIANET_DISABLED_FEATURES="Router2;ReadyEvent"

   # Core transport, RPC and replication, with every optional plugin compiled out
   cmake -B build -DMAFIANET_MINIMAL=ON

The names accepted by ``MAFIANET_DISABLED_FEATURES`` are listed in
``MAFIANET_OPTIONAL_FEATURES`` in ``Source/CMakeLists.txt``; an unknown name
is a configure error. The resulting ``_RAKNET_SUPPORT_<Name>=0`` definitions
are ``PUBLIC`` on the library targets, so code that links ``MafiaNet::MafiaNet``
or ``MafiaNet::MafiaNetStatic`` sees the same header configuration the library
was built with. Plugins that depend on one another are resolved by the header:
disabling ``FileListTransfer`` while keeping ``DirectoryDeltaTransfer`` keeps
``FileListTransfer`` enabled, for instance.

A minimal build is exercised in CI (the ``linux-minimal`` job), which
configures with ``-DMAFIANET_MINIMAL=ON`` and builds both library targets.
The test suites need the plugins, so they are not built in that configuration.

The extras library
------------------

Utilities that are not game networking live in a separate static library,
``MafiaNetExtras`` (``MafiaNet::MafiaNetExtras``), built only when
``MAFIANET_BUILD_EXTRAS=ON``:

* ``EmailSender`` (SMTP client)
* ``HTTPConnection`` and ``HTTPConnection2`` (HTTP clients over ``TCPInterface``)
* ``TelnetTransport`` and ``RakNetTransport2`` (command transports)
* ``ConsoleServer``, ``CommandParserInterface``, ``RakNetCommandParser`` and ``LogCommandParser`` (remote console)
* ``PacketConsoleLogger`` (packet logger writing to a ``LogCommandParser``)

Their headers stay under ``include/mafianet/``, so include paths are unchanged;
only the link line changes. The extras target links the core library
``PUBLIC``, so linking it alone is enough:

.. code-block:: cmake

   find_package(MafiaNet REQUIRED)
   target_link_libraries(my_tool PRIVATE MafiaNet::MafiaNetExtras)

The classes still honour their ``_RAKNET_SUPPORT_<Name>`` flags, and
``MAFIANET_DISABLED_FEATURES`` applies to the extras target too.

Using ``native_feature_includes_overrides.h`` to set the same flags still works,
but the CMake route keeps the configuration in the build system, where it is
visible to consumers through the exported targets.

Batched Datagram I/O
--------------------

On Linux, MafiaNet coalesces multiple datagrams into a single ``recvmmsg(2)`` /
``sendmmsg(2)`` system call instead of one ``recvfrom``/``sendto`` per packet. On a
server pushing high packet rates this removes most of the per-datagram syscall
overhead; at low rates it changes nothing measurable.

**There is nothing to configure.** Batching is a platform capability, not a build
option: it is always on where the syscalls exist, and every other platform compiles
the portable per-datagram paths instead. Delivery semantics are identical either
way -- the same datagrams arrive, in the same order, with the same reliability,
and nothing differs that application code can observe.

Two internals do differ: the number of system calls, and how a transient send
failure is reported inside ``RakNetSocket2::SendBatch``. The batched override
classifies ``errno`` and can report "nothing sent, retry later"; the portable
loop cannot read ``errno`` portably through ``Send()`` and reports every failure
as permanent. Either way the datagrams are dropped and the reliability layer
resends them.

Measured on the 2560-message reliable-ordered burst in
``Tests/Integration/MmsgBatchLiveTests.cpp`` (Linux, Release build, ``strace -c``,
median of 3 runs):

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Syscall
     - Per-datagram
     - Batched
   * - ``sendto``
     - 2907
     - 35
   * - ``sendmmsg``
     - 0
     - 58
   * - ``recvfrom``
     - 2618
     - 0
   * - ``recvmmsg``
     - 0
     - 85
   * - **Total**
     - **5525**
     - **178**

**~31x fewer system calls.** Up to ``MMSG_BATCH_MAX`` (64) datagrams are coalesced
per call.

Counts vary by a percent or two between runs -- congestion control decides how many
datagrams are ready on each tick -- so the ratio is the result, not the exact
figures. Reproduce with:

.. code-block:: bash

   strace -f -c -e trace=sendmmsg,sendto,recvmmsg,recvfrom \
     ./build/Tests/IntegrationTests \
     --gtest_filter='MmsgBatchLive.LargeReliableOrderedBurstArrivesIntactAndInOrder'

The per-datagram column is the code every non-Linux platform runs. To reproduce it
on Linux, flip the ``#if defined(__linux__)`` batching guards in
``socket2.cpp``, ``socket2_berkley.cpp``, ``reliability_layer.cpp`` and
``RakNetSocket2.h`` to ``#if 0``.

CMake presets
-------------

``CMakePresets.json`` (CMake 3.25+) covers the common configurations. Each
preset builds into ``build/<preset>``:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Preset
     - Configuration
   * - ``debug``
     - Debug, samples and tests. ``.clangd`` reads this tree's ``compile_commands.json``.
   * - ``release``
     - Release, libraries only.
   * - ``ci``
     - Release, samples and tests.
   * - ``minimal``
     - Release with ``MAFIANET_MINIMAL=ON``.
   * - ``asan`` / ``tsan``
     - RelWithDebInfo and tests under AddressSanitizer + UBSan, or ThreadSanitizer. Not available on Windows.
   * - ``fuzz``
     - Clang, RelWithDebInfo, the libFuzzer harnesses under ASan + UBSan.
   * - ``fuzz-debug``
     - The same in Debug, so ``RakAssert`` is live: an assert reachable from network input is reported as a crash.
   * - ``bench``
     - Release, the microbenchmarks.
   * - ``coverage``
     - Debug and tests with ``--coverage``.

Workflow presets configure, build and test in one command:

.. code-block:: bash

   cmake --workflow --preset debug
   cmake --workflow --preset asan

   # Or step by step; test presets "unit" and "integration" filter by label
   cmake --preset debug && cmake --build --preset debug
   ctest --preset unit

Sanitizers
----------

The ``asan`` and ``tsan`` presets build with ``RelWithDebInfo`` rather than
``Debug``: ``_DEBUG`` enables ``RakAssert`` checks that abort before the
sanitizer can report, and ``NDEBUG`` is what ships. Their test presets set
``ASAN_OPTIONS`` / ``UBSAN_OPTIONS`` / ``TSAN_OPTIONS``, so run them through
``ctest --preset`` rather than plain ``ctest``.

ThreadSanitizer reports RakNet's by-design unlocked reads of connection state
from the public getters. Those are listed in ``ci/tsan.supp``; the CI TSan job
is advisory and exists to catch new races. On Linux hosts with
``vm.mmap_rnd_bits`` above 28, TSan may fail to start; lower it with
``sudo sysctl vm.mmap_rnd_bits=28``.

On macOS, the ``.devcontainer`` gives a Linux environment for running the
sanitizers and the Linux-only batched datagram paths.

Fuzzing
-------

``Tests/Fuzz/`` holds libFuzzer harnesses for the code that parses what remote
peers send: ``bit_stream`` (BitStream deserialization), ``reliability_layer``
(the datagram parser of a connected ReliabilityLayer) and ``peer_datagram``
(a whole server peer on the simulated network, from offline messages through
a connected session). Build and run them with Clang:

.. code-block:: bash

   cmake --workflow --preset fuzz
   ./build/fuzz/Tests/Fuzz/peer_datagram_fuzz Tests/Fuzz/corpus/peer_datagram -max_total_time=300

Pass a scratch copy of the corpus directory if you do not want new inputs
written into the source tree. Every harness is also built as
``<name>_replay`` with the tests, on every compiler, and replays
``Tests/Fuzz/corpus/<name>/`` as the unit test ``FuzzCorpus.<name>``. When the
fuzzer finds a crash, fix it and add the minimized reproducer
(``-minimize_crash=1``) to that directory so every build keeps checking it.

CI fuzzes each harness for one minute on every pull request, and for fifteen
minutes nightly with a corpus that persists between runs.

Soak tests
----------

``Tests/Soak/`` holds tests that run far longer and wider than the unit and
integration suites, so they are opt-in and carry only the CTest label ``soak``:

* ``SimulatedSoak`` runs one server and 200 encrypted clients on the simulated
  network for more than ten minutes of virtual time (under a minute of
  wall-clock time in Release): per-link loss of 5 to 20 %, latency, jitter
  and reordering, some links capped below the MTU, waves of reconnects and
  crashes from the same address, simultaneous cross-connects, plaintext peers
  that must be refused, and reliable-ordered, split and unreliable traffic. It
  checks in-order delivery, that every connection is keyed, that no settled
  connection drops a datagram as unauthenticated, that every slot is released,
  and that memory does not grow from wave to wave. The scenario is fixed by a
  seed (``MAFIANET_SOAK_SEED``, ``MAFIANET_SOAK_ITERATIONS`` to run several);
  GUIDs and keys still come from the OS, so cross-connect tie-breaks vary.
  ``MAFIANET_SOAK_CLIENTS``, ``MAFIANET_SOAK_WAVES`` (default 8) and
  ``MAFIANET_SOAK_PHASE_SECONDS`` (default 30) scale it.
* ``LoopbackSoak`` runs one server and 64 threaded clients over real loopback
  UDP for ``MAFIANET_SOAK_LOOPBACK_SECONDS`` (default 150) with traffic, churn
  and cross-connects whose two ``Connect()`` calls are made from two threads at
  once.

.. code-block:: bash

   cmake -B build/soak -DCMAKE_BUILD_TYPE=Release -DMAFIANET_BUILD_TESTS=ON -DMAFIANET_BUILD_SOAK_TESTS=ON
   cmake --build build/soak --target SoakTests
   ctest --test-dir build/soak -L soak --output-on-failure

Together they take about three minutes in Release, most of it the loopback
run. CI runs them nightly in Release and under the ``asan`` (with leak
detection) and ``tsan`` presets, with ``MAFIANET_SOAK_WAVES=12``, never on pull
requests.

Benchmarks
----------

.. code-block:: bash

   cmake --preset bench && cmake --build --preset bench
   ./build/bench/Benchmarks/MafiaNetBenchmarks --benchmark_list_tests
   ./build/bench/Benchmarks/MafiaNetBenchmarks --benchmark_filter=RakPeer_UrgentLatency
   ./build/bench/Benchmarks/MafiaNetBenchmarks --benchmark_repetitions=5 \
       --benchmark_out=baseline.json --benchmark_out_format=json

Run the suite manually on the machine you want to measure. CI runs correctness
tests, including the nightly fragmented-transfer reconnect soak. The manual
suite covers the following workloads.

* **Peer bandwidth.** Each send/receive case runs 64 batches. Forward and reverse
  datagrams and UDP payload bytes are averages per batch. The observer includes
  encryption, transport headers, control traffic and the final ACK drain. IP and
  UDP headers are outside the observer. Divide combined UDP payload bytes by
  delivered application bytes to get the reported expansion ratio.
* **Latency under contention.** The simulator sends 100 high-priority probes
  spaced 20 ms apart under a 2 Mibit/s sender cap, with and without a backlog of
  low-priority reliable traffic on a separate ordering channel. Percentiles use
  simulated milliseconds. Bulk goodput counts application bytes delivered during
  the two-second send window. The CPU column measures the work of the simulation.
* **Replication.** The construction case measures the first server tick for
  100 replicas. Steady-state cases change zero, one or all 100 replicas per
  tick, with one or eight clients. ``ReplicaManager3_LargeWorld`` measures a
  warmed world of 50,000 three-float positions, with one or 20 clients and
  zero, 500, or all 50,000 positions changed per tick. ``ReplicaManager3_LargeWorldDirtyList``
  measures the same writes with an application-supplied candidate list. Its time
  includes adding changed positions and copying the list for each connection.
  ``ReplicaManager3_LargeWorldBulkFallback`` measures a known bulk rewrite that
  uses the ordinary scan for that tick. Each case checks one position message
  per changed replica per observer. Bytes per tick count serialized
  RM3 messages at the peer send boundary, before transport framing.
* **Finite path capacity.** ``RakPeer_FiniteBottleneck`` runs the same application
  workload with reliable ordered channels, no sender cap, and a shared FIFO on
  the downstream path. Propagation is 50 ms each way from the handshake. The
  FIFO holds 50 KiB including the packet being serialized and charges IPv4
  and UDP headers. Steady capacity is 256 KiB/s. The step case falls to
  64 KiB/s at 20 seconds and recovers at 40 seconds. Independent 2% loss and
  5% reordering (25 ms extra delay) have separate steady-capacity cases.
  Queue delay is the time an admitted packet waits to start transmission.
  Each phase reports transmitted wire bytes, delivered application bytes,
  drops, and queue occupancy. Cutoff counts and latency percentiles are
  recorded at 60 seconds before checking delivery order and draining both
  directions. ``nak_retry_B`` counts retransmission payload scheduled with a
  deadline advanced by a NAK. Other retries count toward ``rto_retry_B``.
  Window-cut counters count reductions performed while retransmitting each kind.
* **Competing senders.** ``RakPeer_SharedBottleneck`` puts one or two independent
  peers on the same 256 KiB/s FIFO. Each keeps sixteen reliable ordered 16 KiB
  blocks outstanding. The benchmark reports per-flow and aggregate goodput,
  Jain's fairness index, overflow drops, and drain time. The clean run is
  deterministic. The 2% independent loss case uses five matched seeds.
* **Mixed traffic.** ``RakPeer_MixedTraffic`` runs 60 simulated seconds with
  bidirectional gameplay traffic, urgent probes, and a backlogged download.
  Use the same seed with block sizes 0 (download off), 16 and 512 KiB at
  zero or 2% loss. Each seed supplies the same gameplay and urgent workload in
  all six cases. Check
  delivered counts alongside latency. Only messages delivered within the window
  contribute to delivery time. The counters trace enqueue, first transmission,
  retransmission, fragment completion and application delivery. Compare
  ``game_upgraded_to_reliable`` with ``game_unreliable_started`` to see how
  often fragmentation promoted an unreliable gameplay message to reliable.
  Per-class byte counts distinguish offered, first-transmitted, retransmitted
  and delivered application payload. Outstanding bytes include unsent fragments
  of partially transmitted messages. The simulator samples live queued payload every
  100 ms, omitting expired unreliable messages. The oldest queued age is measured at the end of the run.
  Downstream UDP payload bytes include framing, encryption and control traffic.
  With the sliding-window controller, the window, in-flight and available byte
  counters are means sampled over the complete run. The four queue-time buckets
  partition the ticks ending with queued data by exhausted window, exhausted
  application pacing credit, both, or neither.
* **Construction bandwidth.** ``ReplicaManager3_Construct1000`` compares
  unconditional scalar serialization with a full construction snapshot and
  subsequent dirty updates. See :doc:`../plugins/replica-manager-3`.
* **Real UDP loopback.** A threaded peer sends bursts of 128 reliable 900-byte
  messages at 10, 50 or 100 bursts per second. Compare offered bytes/s with
  delivered bytes/s over the send and drain interval. The CPU column covers the
  whole process, including both peers' network threads. On Linux this workload
  exercises the batched socket path.

Numbers only mean something against a baseline on the same machine: run the
benchmarks on both revisions with ``--benchmark_out=<file>
--benchmark_out_format=json`` and compare them with google/benchmark's
``tools/compare.py``.

Coverage
--------

.. code-block:: bash

   cmake --preset coverage && cmake --build --preset coverage
   ctest --preset coverage
   gcovr --root . --filter Source/ build/coverage --html-details coverage/index.html

Running Tests
-------------

Build with tests enabled, then drive the GoogleTest suites through CTest:

.. code-block:: bash

   cmake -B build -DMAFIANET_BUILD_TESTS=ON
   cmake --build build

   ctest --test-dir build --output-on-failure   # everything
   ctest --test-dir build -L unit               # hermetic unit suite only
   ctest --test-dir build -L integration        # loopback integration suite only
   ctest --test-dir build -R "DispatcherLive"   # by name pattern

   # Or run a binary directly with a filter (useful for debugging):
   ./build/Tests/IntegrationTests --gtest_filter='DispatcherLive.*'

See :doc:`../contributing` for the testing guidelines (unit vs. integration,
port allocation, condition-based waiting).

Linking Your Project
--------------------

CMake (recommended):

.. code-block:: cmake

   find_package(MafiaNet REQUIRED)
   target_link_libraries(your_target MafiaNet::MafiaNet)

Manual linking:

* Include path: ``<install_prefix>/include``
* Library path: ``<install_prefix>/lib``
* Link against: ``mafianet`` (shared) or ``MafiaNetStatic`` (static)
