Remote Procedure Calls
======================

Every UDP peer owns one :cpp:class:`MafiaNet::RPC`. Access it with
``peer->GetRPC()`` before or after startup. Its registrations last for the
lifetime of the peer, including across shutdown and restart. Incoming RPCs
run on the thread calling ``Receive()``, after attached message filters.

Migrating from RPC4
-------------------

Include ``mafianet/remote_procedure_call.h`` and replace the RPC4 instance, attachment and
destruction with a reference from ``GetRPC()``. Rename global registrations
to ``RPCGlobalRegistration`` and filter rules to ``MessageFilter::SetAllowRPC``.
Remove ``RPC4Plugin`` from ``MAFIANET_DISABLED_FEATURES``. Calls, signals,
returns and errors retain their payload format.

TCP endpoints enable RPC when the application first calls ``GetRPC()``.
Use ``PacketizedTCP`` for framed remote calls. Blocking calls require a UDP peer.

Basic Usage
-----------

**Registering a function:**

.. code-block:: cpp

   #include "mafianet/guid_util.h"
   #include "mafianet/remote_procedure_call.h"

   void MyCallback(MafiaNet::BitStream* parameters, MafiaNet::Packet* packet, void* context) {
       int value;
       if (!parameters->Read(value))
           return;
       printf("Received value: %d from %s\n", value,
              MafiaNet::to_string(packet->systemAddress).c_str());
   }

   MafiaNet::RPC &rpc = peer->GetRPC();

   // Register a function (pass nullptr when no context is needed)
   rpc.RegisterFunction("MyFunction", MyCallback, nullptr);

**Calling a remote function:**

.. code-block:: cpp

   MafiaNet::BitStream params;
   params.Write(42);

   // Call on a specific system
   rpc.Call("MyFunction", &params, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered,
            0, targetAddress, false);

   // Broadcast to all connected systems
   rpc.Call("MyFunction", &params, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered,
            0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

User Context
------------

Every handler receives the opaque ``void* context`` pointer supplied at
registration time. This lets a handler recover the object instance it belongs
to without resorting to a file-static global pointer:

.. code-block:: cpp

   class ChatBox {
   public:
       void Attach(MafiaNet::RPC* rpc) {
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
is **not owned** by RPC — keep it valid until the function or slot is
unregistered. Pass ``nullptr`` when a handler needs no context.

.. note::

   ``RegisterFunction``, ``RegisterSlot``, ``RegisterBlockingFunction`` and the
   ``RPCGlobalRegistration`` handler constructors all take the context
   parameter, and every handler signature ends with ``void* context``.

Key Features
------------

* String-based function registration and invocation
* Caller-defined parameter encoding via BitStream
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

Ordering Channel
----------------

``Call()``, ``CallBlocking()`` and ``Signal()`` take the ordering channel as an argument and pass it to ``RakPeerInterface::Send()``. Give reliable RPCs a channel of their own rather than the one that carries a sequenced stream: a lost ``ReliableOrdered`` RPC holds back every later sequenced message on its channel until it is retransmitted. RPCs that name objects created through :doc:`../guide/replication` should share that manager's default channel, so an RPC cannot arrive before the object it refers to. See :ref:`ordering-channels`.

See Also
--------

* :doc:`../guide/concepts` - Core networking concepts
* :doc:`../basics/bitstreams` - Parameter serialization
* :doc:`../plugins/fully-connected-mesh-2` - P2P mesh networking with RPC
