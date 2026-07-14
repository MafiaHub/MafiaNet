Quick Start
===========

This guide will help you create a simple client-server application using MafiaNet.

.. tip::
   New code can include the whole core API through the umbrella header
   ``#include "mafianet/mafianet.h"`` and use the modern API — RAII handles,
   startup builders, range-based receive, and typed messages — shown in
   :ref:`modern-quickstart` below. The classic examples that follow use the
   explicit ``GetInstance`` / ``Receive`` / ``DeallocatePacket`` API, which
   remains fully supported. See :doc:`../api/core` for the full reference.

.. _modern-quickstart:

Modern API at a Glance
----------------------

The startup builders, the receive range, the serialization archives, the typed
dispatcher, and typed send/broadcast compose into a complete server without any
manual lifetime or ``BitStream`` bookkeeping:

.. code-block:: cpp

   #include "mafianet/mafianet.h"

   struct ChatMessage {
       std::string text;
       int channel;
       template <class Ar> void serialize(Ar& ar) { ar & text & channel; }
   };

   int main() {
       auto started = MafiaNet::Peer::server()
           .port(60000)
           .max_connections(32)
           .start();                              // Startup + SetMaximumIncomingConnections
       if (!started) {
           printf("startup failed: %d\n", (int) started.error().startup);
           return 1;
       }
       MafiaNet::Peer peer = std::move(*started);  // RAII: destroyed on scope exit

       MafiaNet::Dispatcher d;
       d.on<ChatMessage>([&](const ChatMessage& m, const MafiaNet::Sender& from) {
           printf("%s says: %s\n", from.guid_string().c_str(), m.text.c_str());
           peer.broadcast(d, m);                   // relay to everyone, typed
       });
       d.on(ID_NEW_INCOMING_CONNECTION, [](const MafiaNet::Sender& s) {
           printf("Client connected: %s\n", s.address().ToString());
       });

       while (true)
           for (auto pkt : peer.incoming())        // each packet freed automatically
               d.dispatch(pkt);
   }

Each piece is documented in :doc:`../basics/startup` (builders),
:doc:`../basics/receiving-packets` (receive range and dispatcher),
:doc:`../basics/sending-packets` (typed send/broadcast), and
:doc:`../basics/bitstreams` (the ``serialize()`` convention).

Basic Server
------------

.. code-block:: cpp

   #include "mafianet/peerinterface.h"
   #include "mafianet/MessageIdentifiers.h"

   int main() {
       // Create the peer interface
       MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();

       // Configure socket
       MafiaNet::SocketDescriptor sd(60000, 0);  // Port 60000

       // Start the server
       server->Startup(32, &sd, 1);  // Max 32 connections
       server->SetMaximumIncomingConnections(32);

       printf("Server started on port 60000\n");

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
                       printf("Client disconnected\n");
                       break;
               }
           }
       }

       MafiaNet::RakPeerInterface::DestroyInstance(server);
       return 0;
   }

Basic Client
------------

.. code-block:: cpp

   #include "mafianet/peerinterface.h"
   #include "mafianet/MessageIdentifiers.h"

   int main() {
       MafiaNet::RakPeerInterface* client = MafiaNet::RakPeerInterface::GetInstance();

       MafiaNet::SocketDescriptor sd;
       client->Startup(1, &sd, 1);

       // Connect to server
       client->Connect("127.0.0.1", 60000, nullptr, 0);

       printf("Connecting to server...\n");

       while (true) {
           MafiaNet::Packet* packet;
           for (packet = client->Receive(); packet;
                client->DeallocatePacket(packet), packet = client->Receive()) {

               switch (packet->data[0]) {
                   case ID_CONNECTION_REQUEST_ACCEPTED:
                       printf("Connected to server!\n");
                       break;
                   case ID_CONNECTION_ATTEMPT_FAILED:
                       printf("Connection failed\n");
                       break;
               }
           }
       }

       MafiaNet::RakPeerInterface::DestroyInstance(client);
       return 0;
   }

Sending Custom Messages
-----------------------

Define custom message types:

.. code-block:: cpp

   enum GameMessages {
       ID_GAME_MESSAGE_1 = ID_USER_PACKET_ENUM,
       ID_GAME_MESSAGE_2,
       // Add more as needed
   };

Send a message using BitStream:

.. code-block:: cpp

   #include "mafianet/BitStream.h"

   // Create message
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_GAME_MESSAGE_1);
   bs.Write("Hello, World!");
   bs.Write(42);

   // Send to server (from client) or specific client (from server)
   peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0,
              MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

Next Steps
----------

* :doc:`../guide/concepts` - Learn about MafiaNet concepts
* :doc:`../guide/plugins` - Explore available plugins
* :doc:`../api/core` - API reference
