Preprocessor Directives
=======================

MafiaNet provides numerous preprocessor directives to customize builds for
different platforms, features, and optimization levels.

Configuration File
------------------

Primary configuration is in ``RakNetDefinesOverrides.h``. Create this file to
override defaults from ``RakNetDefines.h``.

.. code-block:: cpp

   // RakNetDefinesOverrides.h
   #ifndef RAKNET_DEFINES_OVERRIDES_H
   #define RAKNET_DEFINES_OVERRIDES_H

   // Your custom defines here
   #define RAKNET_SUPPORT_IPV6 1
   #define USE_SLIDING_WINDOW_CONGESTION_CONTROL 1

   #endif

Core Feature Toggles
--------------------

**Network Features:**

.. code-block:: cpp

   // IPv6 support (default: 0)
   #define RAKNET_SUPPORT_IPV6 1

   // Secure connections with encryption
   #define _RAKNET_SECURE_CONNECTIONS 1

   // Enable statistics tracking
   #define _RAKNET_STAT_COLLECTION 1

**Component Toggles:**

.. code-block:: cpp

   // File operations (FileListTransfer, etc.)
   #define _RAKNET_SUPPORT_FileOperations 1

   // TCP wrapper support
   #define _RAKNET_SUPPORT_PacketizedTCP 1

   // Thread support
   #define _RAKNET_SUPPORT_THREADS 1

Performance Options
-------------------

.. code-block:: cpp

   // Congestion control algorithm
   #define USE_SLIDING_WINDOW_CONGESTION_CONTROL 1

   // Maximum MTU size (default: 1492)
   #define MAXIMUM_MTU_SIZE 1492

   // Packet pool sizes
   #define RAKNET_MESSAGE_HANDLER_LIST_SIZE 32

   // Send buffer size
   #define RAKNET_SEND_BUFFER_SIZE 8192

Debug Options
-------------

.. code-block:: cpp

   // Enable memory tracking
   #define RAKNET_MEMORY_TRACKING 1

   // Verbose debug output
   #define _DEBUG 1

   // Assert on errors
   #define _RAKNET_DEBUG_ASSERT 1

   // Log packet details
   #define _RAKNET_PACKET_LOGGER 1

Platform-Specific
-----------------

.. code-block:: cpp

   // Windows-specific
   #ifdef _WIN32
   #define RAKNET_WINDOWS 1
   #endif

   // Unix/Linux
   #ifdef __linux__
   #define RAKNET_LINUX 1
   #endif

   // macOS
   #ifdef __APPLE__
   #define RAKNET_APPLE 1
   #endif

CMake Options
-------------

CMake build options (passed via ``-D``):

* ``MAFIANET_BUILD_SHARED`` - Build shared library
* ``MAFIANET_BUILD_STATIC`` - Build static library
* ``MAFIANET_BUILD_SAMPLES`` - Build sample applications
* ``MAFIANET_BUILD_TESTS`` - Build test suite

See Also
--------

* :doc:`custom-memory-management` - Memory configuration
* :doc:`ipv6-support` - IPv6 setup
* :doc:`congestion-control` - Bandwidth tuning
