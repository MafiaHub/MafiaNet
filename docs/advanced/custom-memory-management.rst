Custom Memory Management
========================

MafiaNet supports custom memory allocators for fine-grained control over memory
allocation patterns. This is useful for tracking allocations, reducing fragmentation,
or integrating with game engine memory systems.

Overview
--------

Memory management can be customized at multiple levels:

* Global allocation overrides
* Per-system memory pools
* Custom allocators for specific components

Basic Usage
-----------

**Global allocator override:**

.. code-block:: cpp

   #include "slikenet/RakMemoryOverride.h"

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

.. code-block:: cpp

   void* MyMallocTracked(size_t size, const char* file, int line) {
       void* ptr = malloc(size);
       TrackAllocation(ptr, size, file, line);
       return ptr;
   }

   void MyFreeTracked(void* ptr, const char* file, int line) {
       UntrackAllocation(ptr, file, line);
       free(ptr);
   }

   // Enable file/line tracking
   MafiaNet::SetMalloc_Ex(MyMallocTracked);
   MafiaNet::SetFree_Ex(MyFreeTracked);

**Using memory pools:**

.. code-block:: cpp

   #include "slikenet/DS_MemoryPool.h"

   // Create a pool for fixed-size objects
   MafiaNet::DataStructures::MemoryPool<MyPacketData> packetPool;

   // Allocate from pool
   MyPacketData* data = packetPool.Allocate(_FILE_AND_LINE_);

   // Return to pool
   packetPool.Release(data, _FILE_AND_LINE_);

Key Features
------------

* Global malloc/free/realloc override
* File and line number tracking variants
* Built-in memory pool templates
* Thread-safe allocation support
* Memory leak detection helpers

Configuration Macros
--------------------

Define in your build configuration:

* ``_RAKNET_SUPPORT_FileOperations`` - Enable file I/O
* ``_RAKNET_SUPPORT_PacketizedTCP`` - TCP wrapper support
* ``RAKNET_MEMORY_TRACKING`` - Enable allocation tracking

Memory Pool Classes
-------------------

* ``DS_MemoryPool<T>`` - Fixed-size object pool
* ``DS_BytePool`` - Variable-size byte allocation
* ``DS_ThreadSafeAllocatingQueue`` - Thread-safe allocation

See Also
--------

* :doc:`preprocessor-directives` - Build configuration
* :doc:`debugging-disconnects` - Memory-related issues
* :doc:`../api/data-structures` - Container allocations
