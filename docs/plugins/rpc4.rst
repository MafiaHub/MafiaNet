RPC4 Plugin
===========

The RPC4 plugin provides remote procedure call functionality, allowing you to invoke
functions on remote systems as if they were local. This is a simplified version of
the RPC system that uses string-based function identifiers.

Basic Usage
-----------

**Registering a function:**

.. code-block:: cpp

   #include "mafianet/RPC4Plugin.h"

   void MyCallback(MafiaNet::BitStream* parameters, MafiaNet::Packet* packet, void* context) {
       int value;
       parameters->Read(value);
       printf("Received value: %d from %s\n", value,
              packet->systemAddress.ToString());
   }

   // Create and attach the plugin
   MafiaNet::RPC4 rpc;
   peer->AttachPlugin(&rpc);

   // Register a function (pass nullptr when no context is needed)
   rpc.RegisterFunction("MyFunction", MyCallback, nullptr);

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

User Context
------------

Every handler receives the opaque ``void* context`` pointer supplied at
registration time. This lets a handler recover the object instance it belongs
to without resorting to a file-static global pointer:

.. code-block:: cpp

   class ChatBox {
   public:
       void Attach(MafiaNet::RPC4* rpc) {
           // Bind this instance as the context for the handler
           rpc->RegisterSlot("Chat", &ChatBox::OnChat, this, 0);
       }

       static void OnChat(MafiaNet::BitStream* parameters,
                          MafiaNet::Packet* packet, void* context) {
           auto* self = static_cast<ChatBox*>(context);
           self->HandleChat(parameters, packet);
       }

       void HandleChat(MafiaNet::BitStream* parameters, MafiaNet::Packet* packet);
   };

Each registration carries its own context, so the **same** handler may be
registered under **one** identifier for several object instances. The context
is **not owned** by RPC4 — keep it valid until the function or slot is
unregistered. Pass ``nullptr`` when a handler needs no context.

.. note::

   ``RegisterFunction``, ``RegisterSlot``, ``RegisterBlockingFunction`` and the
   ``RPC4GlobalRegistration`` handler constructors all take the context
   parameter, and every handler signature ends with ``void* context``.

Key Features
------------

* String-based function registration and invocation
* Automatic parameter serialization via BitStream
* Per-registration user context passed back to every handler
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
