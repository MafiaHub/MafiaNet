NAT Traversal Architecture
==========================

MafiaNet provides a complete NAT traversal solution combining multiple techniques
to maximize connection success rates between players behind NATs and firewalls.

Overview
--------

The NAT traversal system uses a layered approach:

1. **NAT Type Detection** - Identify NAT characteristics
2. **NAT Punchthrough** - Direct UDP hole punching
3. **UPnP/NATPMP** - Router port mapping
4. **Relay Fallback** - Router2 packet forwarding

Architecture Diagram
--------------------

::

   Client A                    Facilitator                    Client B
      |                            |                             |
      |-- NAT Type Detection ----->|                             |
      |                            |<---- NAT Type Detection ----|
      |                            |                             |
      |-- Punchthrough Request --->|                             |
      |                            |---- Forward Request ------->|
      |<------- Start Punching ----|---- Start Punching -------->|
      |                            |                             |
      |<================ Direct Connection ===================>|
      |                            |                             |
      | (If punchthrough fails)    |                             |
      |<============= Routed via Relay ======================>|

Basic Usage
-----------

**Server (Facilitator) setup:**

.. code-block:: cpp

   #include "slikenet/NatPunchthroughServer.h"
   #include "slikenet/UDPProxyCoordinator.h"

   MafiaNet::NatPunchthroughServer natServer;
   MafiaNet::UDPProxyCoordinator proxyCoord;

   peer->AttachPlugin(&natServer);
   peer->AttachPlugin(&proxyCoord);

**Client NAT punchthrough:**

.. code-block:: cpp

   #include "slikenet/NatPunchthroughClient.h"

   MafiaNet::NatPunchthroughClient natClient;
   peer->AttachPlugin(&natClient);

   // Request punchthrough to target
   natClient.OpenNAT(targetGuid, facilitatorAddress);

   // Handle results
   switch (packet->data[0]) {
       case ID_NAT_PUNCHTHROUGH_SUCCEEDED:
           printf("Direct connection established!\n");
           break;

       case ID_NAT_PUNCHTHROUGH_FAILED:
           printf("Punchthrough failed, using relay\n");
           router2.EstablishRouting(targetGuid);
           break;
   }

**UPnP integration:**

.. code-block:: cpp

   #include "slikenet/NatPunchthroughClient.h"

   // Attempt UPnP port mapping first
   natClient.SetUPNP(true);

   // Or use miniupnpc directly
   #include "miniupnpc/miniupnpc.h"
   // ... UPnP setup code

Key Components
--------------

* **NatPunchthroughClient/Server** - UDP hole punching coordination
* **NatTypeDetectionClient/Server** - NAT behavior analysis
* **Router2** - Relay fallback for failed punchthrough
* **UDPProxyClient/Server/Coordinator** - Managed relay infrastructure

Success Rates by NAT Type
-------------------------

+-------------------+------------------+
| NAT Combination   | Success Rate     |
+===================+==================+
| Full Cone + Any   | ~95%             |
+-------------------+------------------+
| Restricted + Rest.| ~85%             |
+-------------------+------------------+
| Symmetric + Rest. | ~10%             |
+-------------------+------------------+
| Symmetric + Sym.  | ~1% (use relay)  |
+-------------------+------------------+

See Also
--------

* :doc:`../plugins/nat-type-detection` - NAT analysis
* :doc:`../plugins/router2` - Relay fallback
* :doc:`../plugins/fully-connected-mesh-2` - P2P mesh with NAT
