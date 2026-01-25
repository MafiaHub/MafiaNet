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
     - Build test suite

Example with options:

.. code-block:: bash

   cmake -DMAFIANET_BUILD_SAMPLES=ON -DMAFIANET_BUILD_TESTS=ON ..

Running Tests
-------------

Build with tests enabled, then run:

.. code-block:: bash

   ./build/Samples/Tests/Tests

   # Run specific test
   ./build/Samples/Tests/Tests EightPeerTest

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
