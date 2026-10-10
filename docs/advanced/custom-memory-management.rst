Custom Memory Management
========================

MafiaNet lets you replace the allocator it uses for raw buffers, and gives you
object pools for hot allocation paths. This is useful for tracking allocations,
reducing fragmentation, or integrating with a game engine's memory system.

.. important::

   Read :ref:`what-the-override-covers` first. Installing ``SetMalloc`` and
   friends redirects *some* of the library's allocations by default and the rest
   only in a build configured for it, and it never redirects the standard
   containers the library uses internally. A plugin that assumes every byte
   MafiaNet allocates comes from its own heap will be wrong.

Overview
--------

Memory management can be customized at two levels:

* The ``rakMalloc``/``rakRealloc``/``rakFree`` function pointers, used for raw
  buffer allocation and -- in a build with ``_USE_RAK_MEMORY_OVERRIDE=1`` -- for
  object and array allocation through ``OP_NEW``/``OP_NEW_ARRAY``.
* Per-system memory pools (``DataStructures::MemoryPool`` and friends), which you
  can also use for your own fixed-size objects.

Basic Usage
-----------

**Global allocator override:**

.. code-block:: cpp

   #include "mafianet/memory_override.h"

   // Define custom allocation functions
   void* MyMalloc(size_t size) {
       void* ptr = MyGameEngine::Allocate(size, "MafiaNet");
       return ptr;
   }

   void MyFree(void* ptr) {
       MyGameEngine::Free(ptr);
   }

   void* MyRealloc(void* ptr, size_t size) {
       return MyGameEngine::Reallocate(ptr, size, "MafiaNet");
   }

   // Install before any MafiaNet calls
   int main() {
       MafiaNet::SetMalloc(MyMalloc);
       MafiaNet::SetFree(MyFree);
       MafiaNet::SetRealloc(MyRealloc);

       // Now use MafiaNet normally
       MafiaNet::RakPeerInterface* peer =
           MafiaNet::RakPeerInterface::GetInstance();
       // ...
   }

**Allocation tracking:**

The ``_Ex`` variants receive the call site. Note the exact signatures -- ``line``
is ``unsigned int``, and ``SetRealloc_Ex`` must be installed alongside the other
two or reallocations keep going to the default implementation.

.. code-block:: cpp

   void* MyMallocTracked(size_t size, const char* file, unsigned int line) {
       void* ptr = malloc(size);
       TrackAllocation(ptr, size, file, line);
       return ptr;
   }

   void* MyReallocTracked(void* ptr, size_t size, const char* file, unsigned int line) {
       UntrackAllocation(ptr, file, line);
       void* out = realloc(ptr, size);
       TrackAllocation(out, size, file, line);
       return out;
   }

   void MyFreeTracked(void* ptr, const char* file, unsigned int line) {
       UntrackAllocation(ptr, file, line);
       free(ptr);
   }

   // Enable file/line tracking
   MafiaNet::SetMalloc_Ex(MyMallocTracked);
   MafiaNet::SetRealloc_Ex(MyReallocTracked);
   MafiaNet::SetFree_Ex(MyFreeTracked);

MafiaNet ships no leak reporter of its own; tracking is whatever your callbacks
do with the call site. For leak detection, prefer the sanitizers
(``-fsanitize=address``, and LeakSanitizer on Linux), which see the standard
containers too.

**Using memory pools:**

.. code-block:: cpp

   #include "mafianet/ds_memory_pool.h"

   // Create a pool for fixed-size objects
   MafiaNet::DataStructures::MemoryPool<MyPacketData> packetPool;

   // Allocate from pool
   MyPacketData* data = packetPool.Allocate(_FILE_AND_LINE_);

   // Return to pool
   packetPool.Release(data, _FILE_AND_LINE_);

.. _what-the-override-covers:

What the Override Actually Covers
---------------------------------

There are three kinds of allocation inside MafiaNet, and ``SetMalloc`` reaches a
different amount of each.

**1. Raw buffers --- always redirected.**
Library code that allocates a buffer calls ``rakMalloc_Ex`` /
``rakRealloc_Ex`` / ``rakFree_Ex`` directly, so your callbacks are used as soon
as you install them, in every build configuration. Packet payloads, the ban
list's IP strings and ``RakString``'s storage are allocated this way.

**2. Objects and arrays --- only with ``_USE_RAK_MEMORY_OVERRIDE=1``.**
``OP_NEW``, ``OP_NEW_ARRAY`` and ``OP_DELETE`` are the library's object
allocation macros. They route through your allocator only when the library is
compiled with ``_USE_RAK_MEMORY_OVERRIDE=1``:

.. code-block:: cmake

   add_compile_definitions(_USE_RAK_MEMORY_OVERRIDE=1)

**``_USE_RAK_MEMORY_OVERRIDE`` defaults to 0** (see
``Source/include/mafianet/defines.h``), so in a default build every ``OP_NEW``
is a plain ``new`` and your ``SetMalloc`` never sees it. The macro must be
defined identically for the library and for your own translation units, since it
changes what ``OP_NEW`` expands to in the headers.

**3. Standard containers --- never redirected.**
MafiaNet's internals use ``std::vector``, ``std::deque``, ``std::map`` and
``std::unordered_map`` with the default ``std::allocator``. Those allocations go
to the global ``operator new``, in every configuration, and no MafiaNet setter
changes that.

This is not a regression from the old ``DataStructures::List``/``Queue``
containers: those allocated their backing array with ``OP_NEW_ARRAY``, which in a
default build is already a plain ``new Type[count]``. The only configuration in
which the change is observable is one that sets
``_USE_RAK_MEMORY_OVERRIDE=1`` *and* installs a custom allocator. To route the
standard containers too, replace the global ``operator new``/``operator delete``
in your process -- that covers all three categories at once.

Key Features
------------

* Global malloc/realloc/free override for raw buffers
* File and line number tracking variants (``*_Ex``)
* Built-in memory pool templates
* Thread-safe allocating queue

Configuration Macros
--------------------

Define in your build configuration, for the library and your own code alike:

* ``_USE_RAK_MEMORY_OVERRIDE`` - ``1`` routes ``OP_NEW``/``OP_NEW_ARRAY``/
  ``OP_DELETE`` through the installed allocator. Defaults to ``0``.

Memory Pool Classes
-------------------

* ``DataStructures::MemoryPool<T>`` (``mafianet/ds_memory_pool.h``) - fixed-size
  object pool
* ``DataStructures::BytePool`` (``mafianet/ds_byte_pool.h``) - variable-size byte
  allocation, bucketed by size
* ``DataStructures::ThreadsafeAllocatingQueue<T>``
  (``mafianet/ds_threadsafe_allocating_queue.h``) - a queue with a built-in
  free pool, used for cross-thread handoff inside ``RakPeer``

See Also
--------

* :doc:`preprocessor-directives` - Build configuration
* :doc:`debugging-disconnects` - Memory-related issues
* :doc:`../api/data-structures` - Container allocations
