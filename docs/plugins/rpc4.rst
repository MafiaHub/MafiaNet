RPC4 Plugin
===========

The RPC4 plugin provides remote procedure call functionality, allowing you to invoke
functions on remote systems as if they were local. This is a simplified version of
the RPC system that uses string-based function identifiers.

Basic Usage
-----------

**Registering a function:**

.. code-block:: cpp

   #include "slikenet/RPC4Plugin.h"

   void MyCallback(MafiaNet::BitStream* parameters, MafiaNet::Packet* packet) {
       int value;
       parameters->Read(value);
       printf("Received value: %d from %s\n", value,
              packet->systemAddress.ToString());
   }

   // Create and attach the plugin
   MafiaNet::RPC4 rpc;
   peer->AttachPlugin(&rpc);

   // Register a function
   rpc.RegisterFunction("MyFunction", MyCallback);

**Calling a remote function:**

.. code-block:: cpp

   MafiaNet::BitStream params;
   params.Write(42);

   // Call on a specific system
   rpc.Call("MyFunction", &params, HIGH_PRIORITY, RELIABLE_ORDERED,
            0, targetAddress, false);

   // Broadcast to all connected systems
   rpc.Call("MyFunction", &params, HIGH_PRIORITY, RELIABLE_ORDERED,
            0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

Key Features
------------

* String-based function registration and invocation
* Automatic parameter serialization via BitStream
* Support for blocking calls with return values
* Broadcast to all or specific remote systems
* Local function invocation option
* Slot-based callbacks for multiple handlers per function

Configuration Options
---------------------

* ``RegisterFunction()`` - Register a C function callback
* ``RegisterBlockingFunction()`` - Register function that returns a value
* ``RegisterSlot()`` - Register multiple callbacks for same function name
* ``UnregisterFunction()`` - Remove a registered function
* ``CallBlocking()`` - Call and wait for return value

See Also
--------

* :doc:`../guide/concepts` - Core networking concepts
* :doc:`../basics/bitstreams` - Parameter serialization
* :doc:`fully-connected-mesh-2` - P2P mesh networking with RPC
