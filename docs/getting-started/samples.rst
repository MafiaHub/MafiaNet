Samples
=======

MafiaNet includes 80+ sample applications demonstrating various features.

Building Samples
----------------

.. code-block:: bash

   cd build
   cmake -DMAFIANET_BUILD_SAMPLES=ON ..
   cmake --build .

Sample applications are built in ``build/Samples/``.

Beginner Samples
----------------

Ping
~~~~

``Samples/Ping/``

Basic UDP ping demonstration. Shows:

- Starting RakPeerInterface
- Sending/receiving ping packets
- Basic packet handling

.. code-block:: bash

   ./Samples/Ping/Ping

Chat Example
~~~~~~~~~~~~

``Samples/ChatExample/``

Simple chat client and server. Shows:

- Client-server architecture
- Custom message types
- Broadcasting messages

.. code-block:: bash

   # Terminal 1
   ./Samples/ChatExample/ChatExampleServer

   # Terminal 2+
   ./Samples/ChatExample/ChatExampleClient

Intermediate Samples
--------------------

ReplicaManager3
~~~~~~~~~~~~~~~

``Samples/ReplicaManager3/``

Object replication system. Shows:

- Automatic object synchronization
- Serialization/deserialization
- Object creation and destruction

FileListTransfer
~~~~~~~~~~~~~~~~

``Samples/FileListTransfer/``

File transfer with delta compression. Shows:

- Sending multiple files
- Progress callbacks
- Bandwidth management

FullyConnectedMesh
~~~~~~~~~~~~~~~~~~

``Samples/FullyConnectedMesh/`` and ``Samples/FCMHost/``

Peer-to-peer mesh networking. Shows:

- Automatic mesh formation
- Host migration
- Verified connections

RPC4
~~~~

``Samples/RPC4/``

Remote procedure calls. Shows:

- Registering functions
- Calling remote functions
- Parameter passing

Advanced Samples
----------------

NAT Complete Server/Client
~~~~~~~~~~~~~~~~~~~~~~~~~~

``Samples/NATCompleteServer/`` and ``Samples/NATCompleteClient/``

Full NAT traversal demonstration. Shows:

- NAT type detection
- NAT punchthrough
- UPnP fallback
- Router2 relay

Autopatcher
~~~~~~~~~~~

``Samples/AutopatcherServer/`` and ``Samples/AutopatcherClient/``

Delta patching system. Shows:

- Creating patches
- Applying patches
- Version management

Cloud Computing
~~~~~~~~~~~~~~~

``Samples/CloudServer/`` and ``Samples/CloudClient/``

Distributed server architecture. Shows:

- Server-to-server communication
- Load balancing concepts
- Scalability patterns

TeamManager
~~~~~~~~~~~

``Samples/TeamManager/``

Team management for games. Shows:

- Creating teams
- Joining/leaving teams
- Team balancing

Utility Samples
---------------

PacketLogger
~~~~~~~~~~~~

``Samples/PacketLogger/``

Network traffic logging. Shows:

- Logging all packets
- Custom log formatting
- Debug output

ComprehensiveTest
~~~~~~~~~~~~~~~~~

``Samples/ComprehensiveTest/``

Stress testing. Shows:

- Multiple connections
- High packet rates
- Connection stability

Tests
~~~~~

``Samples/Tests/``

Unit and integration tests:

.. code-block:: bash

   ./Samples/Tests/Tests                    # Run all tests
   ./Samples/Tests/Tests EightPeerTest      # Run specific test

Sample Directory Structure
--------------------------

Each sample typically contains:

.. code-block:: text

   Samples/SampleName/
   ├── CMakeLists.txt      # Build configuration
   ├── main.cpp            # Main source (or SampleName.cpp)
   └── README.md           # Sample description (if present)

Running Samples
---------------

Most samples are interactive console applications:

1. Build with ``MAFIANET_BUILD_SAMPLES=ON``
2. Navigate to ``build/Samples/SampleName/``
3. Run the executable
4. Follow on-screen prompts

Many samples require running multiple instances (server + clients) in separate terminals.

See Also
--------

* :doc:`tutorial` - Step-by-step tutorial
* :doc:`quickstart` - Quick code examples
