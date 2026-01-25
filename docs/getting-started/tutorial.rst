Tutorial
========

This tutorial walks you through creating a simple chat application with MafiaNet.

Project Setup
-------------

Create a new project with the following files:

- ``ChatCommon.h`` - Shared definitions
- ``ChatServer.cpp`` - Server application
- ``ChatClient.cpp`` - Client application

Shared Definitions
------------------

Create ``ChatCommon.h``:

.. code-block:: cpp

   #pragma once
   #include "mafianet/MessageIdentifiers.h"

   // Custom message types
   enum ChatMessages {
       ID_CHAT_MESSAGE = ID_USER_PACKET_ENUM,
       ID_SERVER_MESSAGE,
   };

   const unsigned short SERVER_PORT = 60000;
   const unsigned short MAX_CLIENTS = 32;

The Server
----------

Create ``ChatServer.cpp``:

.. code-block:: cpp

   #include "mafianet/peerinterface.h"
   #include "mafianet/BitStream.h"
   #include "mafianet/string.h"
   #include "ChatCommon.h"
   #include <cstdio>

   int main() {
       MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();

       // Start server
       MafiaNet::SocketDescriptor sd(SERVER_PORT, 0);
       MafiaNet::StartupResult result = server->Startup(MAX_CLIENTS, &sd, 1);

       if (result != MafiaNet::RAKNET_STARTED) {
           printf("Failed to start server\n");
           return 1;
       }

       server->SetMaximumIncomingConnections(MAX_CLIENTS);
       printf("Chat server started on port %d\n", SERVER_PORT);

       // Main loop
       while (true) {
           MafiaNet::Packet* packet;
           for (packet = server->Receive(); packet;
                server->DeallocatePacket(packet), packet = server->Receive()) {

               switch (packet->data[0]) {
                   case ID_NEW_INCOMING_CONNECTION:
                       printf("Client connected: %s\n",
                              packet->systemAddress.ToString());
                       break;

                   case ID_DISCONNECTION_NOTIFICATION:
                   case ID_CONNECTION_LOST:
                       printf("Client disconnected: %s\n",
                              packet->systemAddress.ToString());
                       break;

                   case ID_CHAT_MESSAGE: {
                       // Read the message
                       MafiaNet::BitStream bs(packet->data, packet->length, false);
                       bs.IgnoreBytes(1);

                       MafiaNet::RakString message;
                       bs.Read(message);

                       printf("[%s]: %s\n",
                              packet->systemAddress.ToString(),
                              message.C_String());

                       // Broadcast to all clients
                       server->Send(&bs, MEDIUM_PRIORITY, RELIABLE_ORDERED,
                                   0, packet->systemAddress, true);
                       break;
                   }
               }
           }

           // Small sleep to prevent busy loop
           MafiaNet::RakSleep(30);
       }

       MafiaNet::RakPeerInterface::DestroyInstance(server);
       return 0;
   }

The Client
----------

Create ``ChatClient.cpp``:

.. code-block:: cpp

   #include "mafianet/peerinterface.h"
   #include "mafianet/BitStream.h"
   #include "mafianet/string.h"
   #include "mafianet/Gets.h"
   #include "ChatCommon.h"
   #include <cstdio>
   #include <cstring>

   int main() {
       MafiaNet::RakPeerInterface* client = MafiaNet::RakPeerInterface::GetInstance();

       MafiaNet::SocketDescriptor sd;
       client->Startup(1, &sd, 1);

       printf("Enter server IP (or press Enter for localhost): ");
       char serverIP[64];
       Gets(serverIP, sizeof(serverIP));
       if (strlen(serverIP) == 0) {
           strcpy(serverIP, "127.0.0.1");
       }

       printf("Connecting to %s:%d...\n", serverIP, SERVER_PORT);
       client->Connect(serverIP, SERVER_PORT, nullptr, 0);

       bool connected = false;
       char message[256];

       while (true) {
           // Process network packets
           MafiaNet::Packet* packet;
           for (packet = client->Receive(); packet;
                client->DeallocatePacket(packet), packet = client->Receive()) {

               switch (packet->data[0]) {
                   case ID_CONNECTION_REQUEST_ACCEPTED:
                       printf("Connected! Type messages and press Enter.\n");
                       connected = true;
                       break;

                   case ID_CONNECTION_ATTEMPT_FAILED:
                       printf("Connection failed.\n");
                       return 1;

                   case ID_DISCONNECTION_NOTIFICATION:
                   case ID_CONNECTION_LOST:
                       printf("Disconnected.\n");
                       connected = false;
                       return 0;

                   case ID_CHAT_MESSAGE: {
                       MafiaNet::BitStream bs(packet->data, packet->length, false);
                       bs.IgnoreBytes(1);

                       MafiaNet::RakString msg;
                       bs.Read(msg);
                       printf(">> %s\n", msg.C_String());
                       break;
                   }
               }
           }

           // Check for user input (non-blocking)
           if (connected && kbhit()) {
               Gets(message, sizeof(message));
               if (strlen(message) > 0) {
                   MafiaNet::BitStream bs;
                   bs.Write((MafiaNet::MessageID)ID_CHAT_MESSAGE);
                   bs.Write(MafiaNet::RakString(message));
                   client->Send(&bs, MEDIUM_PRIORITY, RELIABLE_ORDERED,
                               0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
               }
           }

           MafiaNet::RakSleep(30);
       }

       MafiaNet::RakPeerInterface::DestroyInstance(client);
       return 0;
   }

Building
--------

Link against the MafiaNet library when compiling:

.. code-block:: bash

   # Linux/macOS
   g++ -std=c++17 ChatServer.cpp -o ChatServer -lmafianet -lpthread
   g++ -std=c++17 ChatClient.cpp -o ChatClient -lmafianet -lpthread

   # Or with CMake
   target_link_libraries(ChatServer MafiaNet::MafiaNet)
   target_link_libraries(ChatClient MafiaNet::MafiaNet)

Running
-------

1. Start the server: ``./ChatServer``
2. Start one or more clients: ``./ChatClient``
3. Type messages in clients and see them broadcast to all

Next Steps
----------

Enhance this example with:

- User names and join/leave notifications
- Private messaging
- Server commands (kick, ban)
- Message history

See Also
--------

* :doc:`quickstart` - Quick code examples
* :doc:`../basics/startup` - Server/client setup details
* :doc:`../guide/client-server` - Client-server patterns
