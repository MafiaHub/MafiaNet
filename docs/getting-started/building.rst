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

Example with options:

.. code-block:: bash

   cmake -DMAFIANET_BUILD_SAMPLES=ON -DMAFIANET_BUILD_TESTS=ON ..

Batched Datagram I/O
--------------------

On Linux, MafiaNet coalesces multiple datagrams into a single ``recvmmsg(2)`` /
``sendmmsg(2)`` system call instead of one ``recvfrom``/``sendto`` per packet. On a
server pushing high packet rates this removes most of the per-datagram syscall
overhead; at low rates it changes nothing measurable.

**There is nothing to configure.** Batching is a platform capability, not a build
option: ``MAFIANET_HAS_MMSG`` (declared in ``mafianet/socket2.h``) is 1 on Linux and
0 on every other platform, where the portable per-datagram paths compile instead.
Behaviour is identical either way -- delivery, ordering and reliability are
unchanged; only the number of system calls differs.

Measured on a 2560-message reliable-ordered burst (Linux, Release build,
``strace -c``):

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Syscall
     - Per-datagram
     - Batched
   * - ``sendto``
     - 2937
     - 36
   * - ``sendmmsg``
     - 0
     - 61
   * - ``recvfrom``
     - 2618
     - 0
   * - ``recvmmsg``
     - 0
     - 87
   * - **Total**
     - **5555**
     - **184**

Up to ``MMSG_BATCH_MAX`` (64) datagrams are coalesced per call.

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
