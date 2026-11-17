Installation
============

Requirements
------------

* CMake 3.21 or higher
* C++17 compatible compiler
* libsodium 1.0.18 or higher (``libsodium-dev`` on Debian/Ubuntu, ``brew install libsodium`` on macOS, ``vcpkg install libsodium`` on Windows)
* pkg-config on Linux and macOS: recommended, not required. ``Findsodium.cmake`` asks it where
  libsodium is installed; without it, libsodium is looked up in the default library and include paths
  (set ``CMAKE_PREFIX_PATH`` for any other location).

Platform-specific requirements:

* **Windows**: Visual Studio 2019 or later, ws2_32 library
* **Linux/macOS**: pthread library

Getting the Source
------------------

Clone the repository:

.. code-block:: bash

   git clone https://github.com/MafiaHub/MafiaNet.git
   cd MafiaNet

Building
--------

See :doc:`building` for detailed build instructions.

Using an installed MafiaNet
---------------------------

``cmake --install`` installs both libraries and a CMake package, so a project only needs
``find_package(MafiaNet REQUIRED)`` with the install prefix in ``CMAKE_PREFIX_PATH``. The package works in a
project that enables only C++ (``project(Game LANGUAGES CXX)``).

.. code-block:: cmake

   find_package(MafiaNet REQUIRED)
   target_link_libraries(game PRIVATE MafiaNet::MafiaNet)        # shared library
   # or: target_link_libraries(game PRIVATE MafiaNet::MafiaNetStatic)

* ``MafiaNet::MafiaNet`` (shared) needs no libsodium development files: the library itself references
  (or, when it was built against a static libsodium, contains) libsodium.
* ``MafiaNet::MafiaNetStatic`` needs them: your program links libsodium too, of the same kind (static or
  shared) MafiaNet was built against, which the package records. If libsodium cannot be found, the package
  still loads, and only a target that links ``MafiaNet::MafiaNetStatic`` fails to configure.

``ci/consumer`` in the repository is a minimal project of this kind; CI builds it against an installed
package on Linux, macOS and Windows.
