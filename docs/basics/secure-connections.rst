Secure Connections
==================

MafiaNet provides encryption and secure authentication for network communications.

Overview
--------

MafiaNet's security features include:

- **Secure handshake**: RSA-based key exchange
- **Encryption**: AES encryption for all traffic after handshake
- **Two-way authentication**: Verify both client and server identity

Enabling Secure Connections
---------------------------

Server Setup
~~~~~~~~~~~~

.. code-block:: cpp

   // Generate or load RSA keys
   // In production, load from file. For testing, generate:
   char publicKey[294];
   char privateKey[1191];
   MafiaNet::GenerateServerRSAKeys(publicKey, privateKey);

   // Initialize security
   peer->InitializeSecurity(publicKey, privateKey, false);

   // Start server normally
   MafiaNet::SocketDescriptor sd(60000, 0);
   peer->Startup(100, &sd, 1);
   peer->SetMaximumIncomingConnections(100);

Client Setup
~~~~~~~~~~~~

.. code-block:: cpp

   // Server's public key (obtained securely - embedded in client, etc.)
   MafiaNet::PublicKey serverPublicKey;
   serverPublicKey.publicKeyMode = MafiaNet::PKM_ACCEPT_ANY_PUBLIC_KEY;
   // Or for strict verification:
   // serverPublicKey.publicKeyMode = MafiaNet::PKM_USE_KNOWN_PUBLIC_KEY;
   // memcpy(serverPublicKey.remoteServerPublicKey, knownServerKey, 294);

   // Connect with public key
   peer->Connect("game.example.com", 60000, nullptr, 0, &serverPublicKey);

Public Key Modes
----------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Mode
     - Description
   * - ``PKM_ACCEPT_ANY_PUBLIC_KEY``
     - Accept any server (vulnerable to MITM)
   * - ``PKM_USE_KNOWN_PUBLIC_KEY``
     - Only accept specified server key
   * - ``PKM_USE_TWO_WAY_AUTHENTICATION``
     - Both client and server verify each other

Two-Way Authentication
----------------------

For maximum security, both client and server can verify each other:

.. code-block:: cpp

   // Server
   peer->InitializeSecurity(serverPublicKey, serverPrivateKey, true);

   // Add known client keys
   peer->AddToSecurityExceptionList("clientPublicKeyHash");

   // Client
   MafiaNet::PublicKey pk;
   pk.publicKeyMode = MafiaNet::PKM_USE_TWO_WAY_AUTHENTICATION;
   memcpy(pk.remoteServerPublicKey, serverPublicKey, 294);
   memcpy(pk.myPublicKey, clientPublicKey, 294);
   memcpy(pk.myPrivateKey, clientPrivateKey, 1191);

   peer->Connect("game.example.com", 60000, nullptr, 0, &pk);

TwoWayAuthentication Plugin
---------------------------

For password-based authentication without exposing the password:

.. code-block:: cpp

   #include "mafianet/TwoWayAuthentication.h"

   MafiaNet::TwoWayAuthentication* auth =
       MafiaNet::TwoWayAuthentication::GetInstance();
   peer->AttachPlugin(auth);

   // Server: add valid passwords
   auth->AddPassword("GamePassword123");

   // Client: initiate authentication after connecting
   case ID_CONNECTION_REQUEST_ACCEPTED:
       auth->Challenge("GamePassword123", remoteAddress);
       break;

   // Handle result
   case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS:
       printf("Authenticated with server!\n");
       break;

   case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE:
       printf("Authentication failed\n");
       peer->CloseConnection(packet->systemAddress, true);
       break;

Handling Security Events
------------------------

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_PUBLIC_KEY_MISMATCH:
           printf("Server's public key doesn't match expected key!\n");
           // Possible MITM attack - do not proceed
           break;

       case ID_OUR_SYSTEM_REQUIRES_SECURITY:
           printf("Server requires secure connection\n");
           break;

       case ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY:
           printf("Must provide public key to connect\n");
           break;
   }

Key Management
--------------

Generating Keys
~~~~~~~~~~~~~~~

.. code-block:: cpp

   char publicKey[294];
   char privateKey[1191];

   // Generate new keypair
   MafiaNet::GenerateServerRSAKeys(publicKey, privateKey);

   // Save to files for production use
   SaveToFile("server.pub", publicKey, 294);
   SaveToFile("server.key", privateKey, 1191);

Loading Keys
~~~~~~~~~~~~

.. code-block:: cpp

   char publicKey[294];
   char privateKey[1191];

   LoadFromFile("server.pub", publicKey, 294);
   LoadFromFile("server.key", privateKey, 1191);

   peer->InitializeSecurity(publicKey, privateKey, false);

Best Practices
--------------

1. **Never transmit private keys** - Private keys stay on the server.

2. **Embed server public key in client** - Don't download it at runtime.

3. **Use PKM_USE_KNOWN_PUBLIC_KEY** - Prevents MITM attacks.

4. **Validate all data** - Encryption doesn't prevent malicious data.

5. **Rotate keys periodically** - Generate new keys for major updates.

Performance Notes
-----------------

- RSA handshake adds ~50-100ms to connection time
- AES encryption adds minimal CPU overhead
- Encrypted packets are slightly larger

See Also
--------

* :doc:`connecting` - Connection basics
* :doc:`../plugins/two-way-authentication` - Password authentication
