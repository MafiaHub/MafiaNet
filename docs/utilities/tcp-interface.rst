TCPInterface
============

The TCPInterface class provides a simple wrapper around TCP sockets for
applications that need reliable, ordered byte streams instead of UDP packets.
It's useful for HTTP requests, database connections, and other TCP-based protocols.

Basic Usage
-----------

**Creating a TCP server:**

.. code-block:: cpp

   #include "slikenet/TCPInterface.h"

   MafiaNet::TCPInterface tcp;

   // Start listening on port 8080
   bool started = tcp.Start(8080, 64);  // port, max connections
   if (!started) {
       printf("Failed to start TCP server\n");
       return;
   }

   // Accept connections and receive data
   while (running) {
       // Check for new connections
       MafiaNet::SystemAddress newConn = tcp.HasNewIncomingConnection();
       if (newConn != MafiaNet::UNASSIGNED_SYSTEM_ADDRESS) {
           printf("New connection from %s\n", newConn.ToString());
       }

       // Check for received data
       MafiaNet::Packet* packet = tcp.Receive();
       if (packet) {
           printf("Received %d bytes from %s\n",
                  packet->length, packet->systemAddress.ToString());
           tcp.DeallocatePacket(packet);
       }

       // Check for disconnections
       MafiaNet::SystemAddress lost = tcp.HasLostConnection();
       if (lost != MafiaNet::UNASSIGNED_SYSTEM_ADDRESS) {
           printf("Lost connection: %s\n", lost.ToString());
       }
   }

**Creating a TCP client:**

.. code-block:: cpp

   MafiaNet::TCPInterface tcp;
   tcp.Start(0, 1);  // Any port, 1 connection

   // Connect to server
   MafiaNet::SystemAddress serverAddr =
       tcp.Connect("example.com", 80, true);  // blocking

   if (serverAddr == MafiaNet::UNASSIGNED_SYSTEM_ADDRESS) {
       printf("Connection failed\n");
       return;
   }

   // Send HTTP request
   const char* request = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
   tcp.Send(request, strlen(request), serverAddr, false);

   // Receive response
   MafiaNet::Packet* response = tcp.Receive();
   if (response) {
       printf("Response: %.*s\n", response->length, response->data);
       tcp.DeallocatePacket(response);
   }

Key Features
------------

* Simple TCP socket wrapper
* Blocking and non-blocking modes
* Multiple simultaneous connections
* Automatic connection management
* Thread-safe send/receive
* SSL/TLS support (when enabled)

Configuration Options
---------------------

* ``Start()`` - Initialize and optionally listen
* ``Connect()`` - Connect to remote host
* ``Send()`` - Send data
* ``Receive()`` - Get received data
* ``CloseConnection()`` - Disconnect
* ``HasNewIncomingConnection()`` - Poll for new clients
* ``HasLostConnection()`` - Poll for disconnections

See Also
--------

* :doc:`email-sender` - SMTP over TCP
* :doc:`console-server` - Remote console via TCP
* :doc:`../basics/bitstreams` - Data serialization
