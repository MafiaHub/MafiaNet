Utilities API Reference
=======================

Strings and spatial helpers that ship with MafiaNet. Containers are the C++ standard
library ones; see :doc:`../getting-started/system-overview`.

Use UTF-8 in ``std::string`` for application text. See :doc:`../basics/creating-packets` for its wire encoding.

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
