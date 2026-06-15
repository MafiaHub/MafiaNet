Data Structures API Reference
=============================

MafiaNet includes several custom data structures optimized for networking.

DS_List
-------

Dynamic array similar to ``std::vector``.

.. doxygenclass:: DataStructures::List
   :members:
   :undoc-members:

DS_Queue
--------

FIFO queue implementation.

.. doxygenclass:: DataStructures::Queue
   :members:
   :undoc-members:

DS_Map
------

Ordered map implementation.

.. doxygenclass:: DataStructures::Map
   :members:
   :undoc-members:

DS_OrderedList
--------------

Sorted list with binary search.

.. doxygenclass:: DataStructures::OrderedList
   :members:
   :undoc-members:

DS_Hash
-------

Hash table implementation.

.. doxygenclass:: DataStructures::Hash
   :members:
   :undoc-members:

DS_MemoryPool
-------------

Memory pool for efficient allocation.

.. doxygenclass:: DataStructures::MemoryPool
   :members:
   :undoc-members:

DS_ByteQueue
------------

Byte queue for streaming data.

.. doxygenclass:: DataStructures::ByteQueue
   :members:
   :undoc-members:

RakString
---------

String class with networking optimizations.

.. doxygenclass:: MafiaNet::RakString
   :members:
   :undoc-members:

RakWString
----------

Wide string class.

.. doxygenclass:: MafiaNet::RakWString
   :members:
   :undoc-members:

PointGridSectorizer
-------------------

A uniform spatial grid over point entries, useful as an interest-management
index (e.g. "which entities are near this position?"). Unlike the append-only
``GridSectorizer``, it keeps a per-entry record (cell + slot) in a
pointer-keyed hash, giving **O(1)** ``RemoveEntry`` and ``MoveEntry`` (via
swap-remove within a cell, with a cheap early-out when a move stays in its
current cell). ``AddEntry`` / ``MoveEntry`` share upsert semantics (one entry
per pointer), and ``GetEntries`` never returns duplicates. Out-of-bounds
positions and query rectangles clamp to the edge cells.

.. doxygenclass:: MafiaNet::PointGridSectorizer
   :members:
   :undoc-members:
