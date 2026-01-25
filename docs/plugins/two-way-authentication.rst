TwoWayAuthentication Plugin
===========================

The TwoWayAuthentication plugin provides mutual authentication between client and
server using password-based challenge-response. Both parties verify each other's
identity without transmitting the password directly.

Basic Usage
-----------

**Server setup:**

.. code-block:: cpp

   #include "slikenet/TwoWayAuthentication.h"

   MafiaNet::TwoWayAuthentication twoWayAuth;
   peer->AttachPlugin(&twoWayAuth);

   // Add valid passwords (stored hashed internally)
   twoWayAuth.AddPassword("admin", "secretAdminPass");
   twoWayAuth.AddPassword("user", "regularUserPass");

**Client authentication:**

.. code-block:: cpp

   MafiaNet::TwoWayAuthentication twoWayAuth;
   peer->AttachPlugin(&twoWayAuth);

   // After connection established, initiate authentication
   void OnConnected(MafiaNet::SystemAddress serverAddr) {
       twoWayAuth.Challenge("user", "regularUserPass", serverAddr);
   }

**Handling authentication results:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       switch (packet->data[0]) {
           case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS:
               printf("Remote system authenticated successfully\n");
               break;

           case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS:
               printf("We authenticated to remote system\n");
               // Both directions verified - connection is secure
               OnFullyAuthenticated(packet->systemAddress);
               break;

           case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE:
               printf("Remote failed to authenticate\n");
               peer->CloseConnection(packet->systemAddress, true);
               break;

           case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE:
               printf("Our authentication failed\n");
               break;

           case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT:
               printf("Authentication timed out\n");
               break;
       }
       peer->DeallocatePacket(packet);
   }

Key Features
------------

* Mutual authentication (both parties verify)
* Password never transmitted (challenge-response)
* Multiple password support with identifiers
* Timeout handling for unresponsive peers
* Replay attack protection
* Integration with message filtering

Security Notes
--------------

* Passwords are hashed before storage
* Challenge-response prevents eavesdropping
* Each authentication uses unique nonce
* Does not encrypt subsequent traffic (use encryption plugin if needed)

Configuration Options
---------------------

* ``AddPassword()`` - Register valid credentials
* ``RemovePassword()`` - Remove credentials
* ``Challenge()`` - Initiate authentication
* ``SetChallengeSendInterval()`` - Retry timing
* ``SetChallengeTimeout()`` - Failure timeout

See Also
--------

* :doc:`message-filter` - Restrict unauthenticated access
* :doc:`../basics/secure-connections` - Encryption options
* :doc:`lobby2` - Higher-level authentication
