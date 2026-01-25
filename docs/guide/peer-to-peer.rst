Peer-to-Peer Networking
=======================

MafiaNet supports peer-to-peer networking with NAT traversal capabilities.

FullyConnectedMesh2
-------------------

The ``FullyConnectedMesh2`` plugin helps establish fully connected peer-to-peer networks.

.. code-block:: cpp

   #include "mafianet/FullyConnectedMesh2.h"

   MafiaNet::FullyConnectedMesh2* fcm2 = MafiaNet::FullyConnectedMesh2::GetInstance();
   peer->AttachPlugin(fcm2);

   // Set connection password (all peers must use the same)
   fcm2->SetConnectOnNewRemoteConnection(true, "GamePassword");

NAT Punchthrough
----------------

For peers behind NAT, use the NAT punchthrough system.

**Server side** (NAT facilitator):

.. code-block:: cpp

   #include "mafianet/NatPunchthroughServer.h"

   MafiaNet::NatPunchthroughServer* natServer =
       MafiaNet::NatPunchthroughServer::GetInstance();
   server->AttachPlugin(natServer);

**Client side**:

.. code-block:: cpp

   #include "mafianet/NatPunchthroughClient.h"

   MafiaNet::NatPunchthroughClient* natClient =
       MafiaNet::NatPunchthroughClient::GetInstance();
   peer->AttachPlugin(natClient);

   // First, connect to the NAT facilitator server
   peer->Connect("nat-server.example.com", 61111, nullptr, 0);

   // Then request punchthrough to another peer
   natClient->OpenNAT(targetGuid, facilitatorAddress);

Handling NAT Events
-------------------

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_NAT_PUNCHTHROUGH_SUCCEEDED:
           // Can now connect directly to the peer
           peer->Connect(packet->systemAddress.ToString(false),
                        packet->systemAddress.GetPort(), nullptr, 0);
           break;

       case ID_NAT_PUNCHTHROUGH_FAILED:
           // NAT punchthrough failed - may need relay
           break;

       case ID_NAT_TARGET_NOT_CONNECTED:
           // Target peer is not connected to facilitator
           break;
   }

UPnP Support
------------

MafiaNet can use UPnP to automatically configure port forwarding:

.. code-block:: cpp

   // Attempt to open port via UPnP (requires miniupnpc)
   peer->SetInternalID(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, 0);
