BitStreams
==========

The ``BitStream`` class is a helper class used to pack and unpack bits into a dynamic array. It's the primary way to serialize network data in MafiaNet.

Key Benefits
------------

1. **Dynamic packet creation** - Build packets at runtime instead of using fixed structs
2. **Compression** - Compress native types using specialized methods
3. **Bit-level writing** - Write individual bits (booleans use only 1 bit)
4. **Endian swapping** - Handle cross-platform byte order differences

Writing Data
------------

BitStream is templated to accept any data type:

.. code-block:: cpp

   #include "mafianet/BitStream.h"

   MafiaNet::BitStream bs;

   // Write primitive types
   bs.Write((MafiaNet::MessageID)ID_GAME_MESSAGE);
   bs.Write(42);                    // int
   bs.Write(3.14f);                 // float
   bs.Write("Hello");               // C-string
   bs.Write(true);                  // bool (uses only 1 bit!)

   // Write with compression (smaller bandwidth for small values)
   bs.WriteCompressed(health);      // Uses variable encoding

Writing Structs
~~~~~~~~~~~~~~~

You can write entire structs:

.. code-block:: cpp

   struct MyVector {
       float x, y, z;
   } myVector;

   // No endian swapping - fast but not cross-platform safe
   bitStream.Write(myVector);

   // With endian swapping - cross-platform safe
   bitStream.Write(myVector.x);
   bitStream.Write(myVector.y);
   bitStream.Write(myVector.z);

Custom Serialization Operators
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Override shift operators for custom types:

.. code-block:: cpp

   namespace MafiaNet {

   BitStream& operator<<(BitStream& out, MyVector& in) {
       out.WriteNormVector(in.x, in.y, in.z);
       return out;
   }

   BitStream& operator>>(BitStream& in, MyVector& out) {
       in.ReadNormVector(out.x, out.y, out.z);
       return in;
   }

   } // namespace MafiaNet

   // Usage
   myVector << bitStream;  // Read from bitstream
   myVector >> bitStream;  // Write to bitstream

Reading Data
------------

Create a BitStream from received packet data:

.. code-block:: cpp

   // From a received Packet*
   MafiaNet::BitStream bs(packet->data, packet->length, false);

   // Skip the message ID (first byte)
   bs.IgnoreBytes(1);

   // Read data in the same order it was written
   int value;
   float position;
   bs.Read(value);
   bs.Read(position);

   // Read with compression
   int health;
   bs.ReadCompressed(health);

The third parameter (``false``) means don't copy the data - just reference it. Use ``true`` if you need the BitStream to own the data.

Specialized Write Methods
-------------------------

MafiaNet provides optimized methods for common game data:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Description
   * - ``WriteCompressed()``
     - Variable-length encoding for integers
   * - ``WriteNormVector()``
     - Normalized 3D vector (uses less bandwidth)
   * - ``WriteVector()``
     - 3D vector with magnitude
   * - ``WriteNormQuat()``
     - Normalized quaternion
   * - ``WriteOrthMatrix()``
     - Orthonormal rotation matrix
   * - ``WriteBitsFromIntegerRange()``
     - Integer within known range
   * - ``WriteFloat16()``
     - 16-bit float (half precision)
   * - ``WriteAlignedBytes()``
     - Byte-aligned data block

Pre-allocating Size
-------------------

If you know approximate packet size, pre-allocate to avoid reallocations:

.. code-block:: cpp

   MafiaNet::BitStream bs(256);  // Pre-allocate 256 bytes

Checking Read Success
---------------------

Read methods return ``true`` on success:

.. code-block:: cpp

   float x, y, z;
   if (!bs.Read(x) || !bs.Read(y) || !bs.Read(z)) {
       // Packet was truncated or corrupted
       return;
   }

Getting Raw Data
----------------

Access the underlying buffer:

.. code-block:: cpp

   unsigned char* data = bs.GetData();
   BitSize_t numBits = bs.GetNumberOfBitsUsed();
   BitSize_t numBytes = bs.GetNumberOfBytesUsed();

See Also
--------

* :doc:`creating-packets` - Creating custom packets
* :doc:`sending-packets` - Sending packets
* :doc:`receiving-packets` - Receiving and parsing packets
