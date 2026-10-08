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
     - Semicolon-separated plugin names to compile out, e.g. ``"ReplicaManager3;RPC4Plugin"``. See `Minimal builds`_.
   * - ``MAFIANET_MINIMAL``
     - OFF
     - Compile out every optional plugin, leaving the core transport only. See `Minimal builds`_.

Example with options:

.. code-block:: bash

   cmake -DMAFIANET_BUILD_SAMPLES=ON -DMAFIANET_BUILD_TESTS=ON ..

Minimal builds
--------------

Every optional plugin in the core library (``ReplicaManager3``, ``RPC4Plugin``,
``FileListTransfer``, the NAT traversal plugins, ``TCPInterface`` and its
users, and so on) is guarded by a ``_RAKNET_SUPPORT_<Name>`` flag declared in
``mafianet/native_feature_includes.h``. The transport itself, ``BitStream``,
``RakPeer``, the reliability layer and ``RakVoice`` are always built.

The supported way to compile plugins out is through CMake, not by editing the
header:

.. code-block:: bash

   # Drop specific plugins
   cmake -B build -DMAFIANET_DISABLED_FEATURES="ReplicaManager3;RPC4Plugin;TeamManager"

   # Core transport only: every optional plugin compiled out
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
