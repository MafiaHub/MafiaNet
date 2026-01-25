StringCompressor
================

The StringCompressor utility provides efficient compression of string data using
Huffman encoding with frequency tables optimized for common text patterns. It
significantly reduces bandwidth for text-heavy traffic.

Basic Usage
-----------

**Encoding strings:**

.. code-block:: cpp

   #include "slikenet/StringCompressor.h"
   #include "slikenet/BitStream.h"

   // Get the global instance
   MafiaNet::StringCompressor* compressor =
       MafiaNet::StringCompressor::Instance();

   // Compress a string into a BitStream
   MafiaNet::BitStream bs;
   compressor->EncodeString("Hello, World!", 256, &bs);

   // The BitStream now contains the compressed string
   printf("Compressed size: %d bits\n", bs.GetNumberOfBitsUsed());

**Decoding strings:**

.. code-block:: cpp

   // Decompress from BitStream
   char buffer[256];
   compressor->DecodeString(buffer, 256, &bs);

   printf("Decoded: %s\n", buffer);  // "Hello, World!"

**Using with network packets:**

.. code-block:: cpp

   // Sender
   void SendChatMessage(const char* message, MafiaNet::SystemAddress target) {
       MafiaNet::BitStream bs;
       bs.Write((MafiaNet::MessageID)ID_CHAT_MESSAGE);

       MafiaNet::StringCompressor::Instance()->EncodeString(
           message, 512, &bs);

       peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, target, false);
   }

   // Receiver
   void HandleChatMessage(MafiaNet::Packet* packet) {
       MafiaNet::BitStream bs(packet->data, packet->length, false);

       MafiaNet::MessageID msgId;
       bs.Read(msgId);

       char message[512];
       MafiaNet::StringCompressor::Instance()->DecodeString(
           message, 512, &bs);

       DisplayChat(message);
   }

**Custom frequency tables:**

.. code-block:: cpp

   // Create custom frequency table for specific text patterns
   // (e.g., game-specific terminology)

   // Add reference strings to build frequency table
   compressor->AddReference("attack");
   compressor->AddReference("defend");
   compressor->AddReference("health");
   compressor->AddReference("mana");

   // Generate optimized encoding table
   compressor->GenerateTreeFromStrings();

Key Features
------------

* Huffman encoding for text compression
* Pre-built frequency tables for English text
* Custom frequency table support
* BitStream integration
* Thread-safe singleton instance
* Significant bandwidth savings for chat/text

Compression Ratios
------------------

Typical compression ratios:

* English text: 50-60% reduction
* Game commands: 40-50% reduction
* Player names: 30-40% reduction
* URLs/paths: 20-30% reduction

Configuration Options
---------------------

* ``Instance()`` - Get singleton instance
* ``EncodeString()`` - Compress string
* ``DecodeString()`` - Decompress string
* ``AddReference()`` - Add to frequency analysis
* ``GenerateTreeFromStrings()`` - Build custom table

See Also
--------

* :doc:`../basics/bitstreams` - Binary serialization
* :doc:`../plugins/file-list-transfer` - File compression
* :doc:`../advanced/congestion-control` - Bandwidth management
