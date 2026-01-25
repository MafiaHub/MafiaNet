PacketLogger Plugin
===================

The PacketLogger plugin provides detailed logging of all network traffic for
debugging and analysis. It can output packet information to the console, file,
or custom handlers.

Basic Usage
-----------

**Basic packet logging:**

.. code-block:: cpp

   #include "slikenet/PacketLogger.h"

   MafiaNet::PacketLogger packetLogger;
   peer->AttachPlugin(&packetLogger);

   // Enable logging (enabled by default)
   packetLogger.SetLogDirect(true);

   // Log to file instead of console
   packetLogger.SetLogFile("network_log.txt");

**Custom log handler:**

.. code-block:: cpp

   class MyPacketLogger : public MafiaNet::PacketLogger {
   public:
       void WriteLog(const char* str) override {
           // Custom logging - write to your game's log system
           MyGameLog::Write("[NET] %s", str);
       }

       void FormatLine(char* output, const char* dir,
                       const char* type, unsigned int packetNum,
                       unsigned long long time, unsigned int local,
                       unsigned int remote, unsigned int bytes,
                       unsigned int seqNum) override {
           // Custom format
           sprintf(output, "[%llu] %s %s (%d bytes)",
                   time, dir, type, bytes);
       }
   };

   MyPacketLogger myLogger;
   peer->AttachPlugin(&myLogger);

**Filtering logged messages:**

.. code-block:: cpp

   // Log only specific message types
   packetLogger.SetLogMessageType(ID_CONNECTION_REQUEST, true);
   packetLogger.SetLogMessageType(ID_USER_PACKET_ENUM, true);

   // Disable logging of frequent messages
   packetLogger.SetLogMessageType(ID_CONNECTED_PING, false);
   packetLogger.SetLogMessageType(ID_CONNECTED_PONG, false);

Key Features
------------

* Detailed packet information logging
* Send and receive direction tracking
* Timestamp and sequence number tracking
* Configurable output formats
* File and console output options
* Message type filtering
* Custom log handler support

Log Output Format
-----------------

Default log format includes:

* Direction (Send/Receive)
* Message type name
* Packet number
* Timestamp
* Local/remote port
* Byte count
* Sequence number

Example output::

   Snd ID_CONNECTION_REQUEST #1 0.000s 60000->60001 46 bytes seq=0
   Rcv ID_CONNECTION_ACCEPTED #2 0.015s 60001->60000 28 bytes seq=0

Configuration Options
---------------------

* ``SetLogDirect()`` - Enable/disable logging
* ``SetLogFile()`` - Log to file
* ``SetLogMessageType()`` - Filter by message type
* ``SetPrintID()`` - Include message ID in output
* ``SetPrintAcks()`` - Log acknowledgment packets

See Also
--------

* :doc:`message-filter` - Filter unwanted packets
* :doc:`../advanced/debugging-disconnects` - Connection troubleshooting
* :doc:`../basics/network-messages` - Message ID reference
