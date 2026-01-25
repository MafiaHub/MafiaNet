Data Compression
================

MafiaNet provides several compression techniques to reduce bandwidth usage.

BitStream Compression
---------------------

WriteCompressed
~~~~~~~~~~~~~~~

For integers that are typically small:

.. code-block:: cpp

   // Regular write: always 4 bytes
   bs.Write(health);  // int = 4 bytes

   // Compressed: 1-5 bytes depending on value
   bs.WriteCompressed(health);  // Small values use fewer bytes

   // Reading
   bs.ReadCompressed(health);

Best for: health, ammo, scores, small counts.

Normalized Vectors
~~~~~~~~~~~~~~~~~~

For unit vectors (direction):

.. code-block:: cpp

   // Regular: 12 bytes (3 floats)
   bs.Write(dirX);
   bs.Write(dirY);
   bs.Write(dirZ);

   // Compressed: ~6 bytes
   bs.WriteNormVector(dirX, dirY, dirZ);

   // Reading
   bs.ReadNormVector(dirX, dirY, dirZ);

Quaternions
~~~~~~~~~~~

For rotations:

.. code-block:: cpp

   // Regular: 16 bytes (4 floats)
   // Compressed: ~6 bytes
   bs.WriteNormQuat(w, x, y, z);
   bs.ReadNormQuat(w, x, y, z);

Half-Precision Floats
~~~~~~~~~~~~~~~~~~~~~

For values where full precision isn't needed:

.. code-block:: cpp

   // Regular: 4 bytes
   // Half precision: 2 bytes
   bs.WriteFloat16(value, minValue, maxValue);
   bs.ReadFloat16(value, minValue, maxValue);

Integer Ranges
~~~~~~~~~~~~~~

When you know the value's range:

.. code-block:: cpp

   // Health: 0-100, only needs 7 bits
   bs.WriteBitsFromIntegerRange(health, 0, 100);
   health = bs.ReadBitsFromIntegerRange(0, 100);

   // Team: 0-3, only needs 2 bits
   bs.WriteBitsFromIntegerRange(team, 0, 3);

String Compression
------------------

Use StringCompressor for repeated strings:

.. code-block:: cpp

   #include "mafianet/StringCompressor.h"

   // Add common strings to table
   MafiaNet::StringCompressor::Instance()->AddReference(
       "PlayerJoined", 0);
   MafiaNet::StringCompressor::Instance()->AddReference(
       "PlayerLeft", 1);

   // Encode (uses table index if found)
   MafiaNet::StringCompressor::Instance()->EncodeString(
       "PlayerJoined", 256, &bs);

   // Decode
   char decoded[256];
   MafiaNet::StringCompressor::Instance()->DecodeString(
       decoded, 256, &bs);

DataCompressor
--------------

For larger data blocks:

.. code-block:: cpp

   #include "mafianet/DataCompressor.h"

   // Compress
   unsigned compressedSize;
   unsigned char* compressed = MafiaNet::DataCompressor::Compress(
       data, dataSize, &compressedSize);

   // Decompress
   unsigned decompressedSize;
   unsigned char* decompressed = MafiaNet::DataCompressor::Decompress(
       compressed, compressedSize, &decompressedSize);

Best Practices
--------------

1. **Profile first** - Measure actual bandwidth before optimizing
2. **Compress what matters** - Focus on frequently sent data
3. **Match precision to needs** - Don't send full floats for position that only needs centimeter precision
4. **Use delta compression** - Send only what changed
5. **Batch small messages** - Combine multiple small updates

Compression Comparison
----------------------

.. list-table::
   :header-rows: 1
   :widths: 30 20 20 30

   * - Data Type
     - Uncompressed
     - Compressed
     - Savings
   * - int (small)
     - 4 bytes
     - 1-2 bytes
     - 50-75%
   * - Normalized vector
     - 12 bytes
     - 6 bytes
     - 50%
   * - Quaternion
     - 16 bytes
     - 6 bytes
     - 62%
   * - Float (range known)
     - 4 bytes
     - 2 bytes
     - 50%

See Also
--------

* :doc:`bitstreams` - BitStream basics
* :doc:`../utilities/string-compressor` - String table compression
