Installation
============

Requirements
------------

* CMake 3.21 or higher
* C++17 compatible compiler
* libsodium 1.0.19 or higher (``libsodium-dev`` on Debian/Ubuntu, ``brew install libsodium`` on macOS, ``vcpkg install libsodium`` on Windows)
* OpenSSL 3.0 or higher (until the legacy crypto code is removed)

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
