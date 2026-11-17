Custom Memory Management
========================

MafiaNet allocates with plain ``operator new`` / ``operator delete`` and
``std::malloc`` / ``std::realloc`` / ``std::free``. There is no library-private
allocator layer, so any tool that works on a normal C++ program works here too.

Customising the allocator
-------------------------

To track, pool or otherwise control MafiaNet's allocations, replace the
allocator for the whole process rather than for the library alone:

* Replace the global ``operator new`` and ``operator delete`` in your
  application (see ``<new>``). Every ``new`` / ``delete`` in MafiaNet goes
  through your replacement.
* Link a drop-in ``malloc`` replacement such as mimalloc or jemalloc. This
  also covers the ``std::malloc`` / ``std::free`` calls.
* Use the allocation profilers and sanitizers of your platform
  (AddressSanitizer, Valgrind, Instruments, heaptrack) for leak and
  allocation tracking.

.. code-block:: cpp

   #include <cstdlib>
   #include <new>

   void* operator new(std::size_t size)
   {
       void* ptr = std::malloc(size);
       if (!ptr)
           throw std::bad_alloc();
       return ptr;
   }

   void operator delete(void* ptr) noexcept
   {
       std::free(ptr);
   }

Replace the sized and array forms (``operator new[]``, ``operator delete[]``,
``operator delete(void*, std::size_t)``) as well, or the default forms will
bypass your allocator.

Memory across a DLL boundary
----------------------------

Memory must be freed by the module that allocated it. Create and destroy
library objects with ``GetInstance()`` / ``DestroyInstance()`` and release
received packets with ``DeallocatePacket()``; never ``delete`` them yourself.

Removed API
-----------

The following were removed in this release together with the
``_USE_RAK_MEMORY_OVERRIDE`` build flag and the bundled dlmalloc
(``UseRaknetFixedHeap``):

* ``SetMalloc`` / ``SetRealloc`` / ``SetFree`` and their ``_Ex`` variants
* ``SetNotifyOutOfMemory``
* ``GetMalloc`` / ``GetRealloc`` / ``GetFree``
* ``rakMalloc`` / ``rakRealloc`` / ``rakFree``, ``OP_NEW`` / ``OP_DELETE``
* the ``_FILE_AND_LINE_`` macro and the ``file, line`` allocation-tracking parameters it fed
* the ``BUFFERED_PACKETS_PAGE_SIZE`` and ``INTERNAL_PACKET_PAGE_SIZE`` memory pool page sizes

Replace calls to them with the global allocator replacement described above.

See Also
--------

* :doc:`migrating-to-std-containers` - Upgrading code that used the removed containers and allocators
* :doc:`preprocessor-directives` - Build configuration
* :doc:`debugging-disconnects` - Memory-related issues
