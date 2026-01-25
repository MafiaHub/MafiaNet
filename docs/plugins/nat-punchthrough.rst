NAT Punchthrough
================

NAT Punchthrough enables peer-to-peer connections between clients behind NAT routers.

Overview
--------

Most home internet connections use NAT (Network Address Translation). Direct P2P connections fail because:

1. External IP is shared among devices
2. Router blocks unsolicited incoming packets
3. Port mappings are dynamic

NAT Punchthrough solves this using a facilitator server.

Architecture
------------

.. code-block:: text

   Client A (behind NAT)     Facilitator Server     Client B (behind NAT)
          │                         │                        │
          ├────── Register ────────►│                        │
          │                         │◄────── Register ───────┤
          │                         │                        │
          ├─── Request Punch to B ─►│                        │
          │                         │──── Notify A wants ───►│
          │                         │        to connect      │
          │                         │                        │
          │◄──── Punch timing ──────┤─── Punch timing ──────►│
          │                         │                        │
          ├──────── UDP Punch ──────┼────────────────────────┤
          │◄─────── UDP Punch ──────┼────────────────────────┤
          │                         │                        │
          │◄════════ Direct P2P Connection ═════════════════►│

Server Setup (Facilitator)
--------------------------

.. code-block:: cpp

   #include "mafianet/NatPunchthroughServer.h"

   MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();

   // Attach NAT punchthrough server
   MafiaNet::NatPunchthroughServer* natServer =
       MafiaNet::NatPunchthroughServer::GetInstance();
   server->AttachPlugin(natServer);

   // Start server
   MafiaNet::SocketDescriptor sd(NAT_SERVER_PORT, 0);
   server->Startup(1000, &sd, 1);
   server->SetMaximumIncomingConnections(1000);

Client Setup
------------

.. code-block:: cpp

   #include "mafianet/NatPunchthroughClient.h"

   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   // Attach NAT punchthrough client
   MafiaNet::NatPunchthroughClient* natClient =
       MafiaNet::NatPunchthroughClient::GetInstance();
   peer->AttachPlugin(natClient);

   // Start peer
   MafiaNet::SocketDescriptor sd;
   peer->Startup(8, &sd, 1);

   // Connect to facilitator
   peer->Connect(NAT_SERVER_IP, NAT_SERVER_PORT, nullptr, 0);

Initiating Punchthrough
-----------------------

After connecting to the facilitator:

.. code-block:: cpp

   // Get target's GUID (from matchmaking, lobby, etc.)
   MafiaNet::RakNetGUID targetGuid = GetTargetFromLobby();

   // Initiate punchthrough
   MafiaNet::SystemAddress facilitatorAddr = GetFacilitatorAddress();
   natClient->OpenNAT(targetGuid, facilitatorAddr);

Handling Results
----------------

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_NAT_PUNCHTHROUGH_SUCCEEDED: {
           // Can now connect directly!
           MafiaNet::SystemAddress addr = packet->systemAddress;
           printf("NAT punch succeeded to %s\n", addr.ToString());

           // Connect directly
           peer->Connect(addr.ToString(false), addr.GetPort(),
                        nullptr, 0);
           break;
       }

       case ID_NAT_PUNCHTHROUGH_FAILED: {
           MafiaNet::RakNetGUID failedGuid;
           MafiaNet::BitStream bs(packet->data, packet->length, false);
           bs.IgnoreBytes(1);
           bs.Read(failedGuid);

           printf("NAT punch failed to %s\n", failedGuid.ToString());

           // Consider using Router2 as fallback
           break;
       }

       case ID_NAT_TARGET_NOT_CONNECTED:
           printf("Target not connected to facilitator\n");
           break;

       case ID_NAT_ALREADY_IN_PROGRESS:
           printf("Punchthrough already in progress\n");
           break;
   }

NAT Types
---------

Not all NAT types can be punched through:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Type
     - Punchthrough Success
   * - Full Cone
     - Always succeeds
   * - Address Restricted
     - Usually succeeds
   * - Port Restricted
     - Usually succeeds
   * - Symmetric
     - May fail (use Router2)

Use :doc:`nat-type-detection` to check NAT types before attempting.

Best Practices
--------------

1. **Always have a fallback**: Use Router2 when punchthrough fails
2. **Detect NAT type first**: Warn users with symmetric NAT
3. **Use multiple facilitators**: For redundancy
4. **Timeout handling**: Set reasonable timeouts

See Also
--------

* :doc:`nat-type-detection` - Detect NAT type
* :doc:`router2` - Fallback when punchthrough fails
* :doc:`../advanced/nat-traversal-architecture` - Complete NAT solution
