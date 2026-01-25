Client-Server Architecture
==========================

The client-server model is the most common networking pattern for games.

Server Setup
------------

.. code-block:: cpp

   MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();

   // Bind to port 60000
   MafiaNet::SocketDescriptor sd(60000, 0);

   // Start with capacity for 100 clients
   MafiaNet::StartupResult result = server->Startup(100, &sd, 1);
   if (result != MafiaNet::RAKNET_STARTED) {
       printf("Server failed to start: %d\n", result);
       return -1;
   }

   // Allow incoming connections
   server->SetMaximumIncomingConnections(100);

Client Setup
------------

.. code-block:: cpp

   MafiaNet::RakPeerInterface* client = MafiaNet::RakPeerInterface::GetInstance();

   MafiaNet::SocketDescriptor sd;
   client->Startup(1, &sd, 1);

   // Connect to server
   MafiaNet::ConnectionAttemptResult result =
       client->Connect("game.example.com", 60000, nullptr, 0);

   if (result != MafiaNet::CONNECTION_ATTEMPT_STARTED) {
       printf("Failed to initiate connection\n");
   }

Handling Events
---------------

Common network events to handle:

.. code-block:: cpp

   void ProcessPackets(MafiaNet::RakPeerInterface* peer) {
       MafiaNet::Packet* packet;
       for (packet = peer->Receive(); packet;
            peer->DeallocatePacket(packet), packet = peer->Receive()) {

           switch (packet->data[0]) {
               // Connection events
               case ID_NEW_INCOMING_CONNECTION:  // Server: new client connected
                   OnClientConnected(packet->systemAddress);
                   break;

               case ID_CONNECTION_REQUEST_ACCEPTED:  // Client: connected to server
                   OnConnectedToServer();
                   break;

               case ID_CONNECTION_ATTEMPT_FAILED:
                   OnConnectionFailed();
                   break;

               case ID_NO_FREE_INCOMING_CONNECTIONS:
                   OnServerFull();
                   break;

               // Disconnection events
               case ID_DISCONNECTION_NOTIFICATION:
                   OnDisconnected(packet->systemAddress);
                   break;

               case ID_CONNECTION_LOST:
                   OnConnectionLost(packet->systemAddress);
                   break;

               // Custom game messages
               default:
                   if (packet->data[0] >= ID_USER_PACKET_ENUM) {
                       ProcessGameMessage(packet);
                   }
                   break;
           }
       }
   }

Broadcasting to All Clients
---------------------------

.. code-block:: cpp

   void BroadcastToAllClients(MafiaNet::RakPeerInterface* server,
                              MafiaNet::BitStream* bs) {
       server->Send(bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                    MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
   }

Sending to Specific Client
--------------------------

.. code-block:: cpp

   void SendToClient(MafiaNet::RakPeerInterface* server,
                     MafiaNet::BitStream* bs,
                     MafiaNet::SystemAddress client) {
       server->Send(bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, client, false);
   }

Connection Timeout
------------------

Configure connection timeout behavior:

.. code-block:: cpp

   // Set timeout to 10 seconds
   peer->SetTimeoutTime(10000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
