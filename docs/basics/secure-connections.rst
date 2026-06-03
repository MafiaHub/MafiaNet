Secure Connections
==================

As of MafiaNet 0.8.0, **all connections are encrypted and authenticated by
default** using the `Noise_NK`_ protocol pattern built on **libsodium**.  There
is no opt-out switch: every peer-to-peer datagram is encrypted, integrity-
protected, and replay-guarded from the moment the handshake completes.

.. _Noise_NK: https://noiseprotocol.org/

.. note::

   **Breaking change from 0.7.x.** The wire protocol is incompatible with
   older peers.  The former ``InitializeSecurity`` / ``PublicKey`` /
   ``PublicKeyMode`` API has been removed.  See the
   :ref:`migration guide <secure-connections-migration>` below.

Overview
--------

MafiaNet uses the ``Noise_NK_25519_ChaChaPoly_SHA512`` cipher suite:

* **Key exchange**: X25519 ephemeral DH (client) × X25519 static DH (server).
  Each connection generates fresh ephemeral keys — forward secrecy is built in.
* **Transport encryption**: ChaCha20-Poly1305 AEAD per datagram.
* **Authentication**: the client *pins* the server's 32-byte X25519 public key
  and verifies the server's identity during the handshake.  A server that does
  not hold the matching secret key cannot complete the handshake.
* **DoS mitigation**: a stateless HMAC return-routability cookie gates the
  server's handshake work (similar in spirit to DTLS HelloVerifyRequest).
* **Replay protection**: a 64-entry sliding-window filter drops duplicated or
  replayed datagrams after decryption.

The client is **anonymous** — no client key is exchanged or required.

Dependency: libsodium
---------------------

The Noise handshake and ChaCha20-Poly1305 transport are implemented on top of
`libsodium <https://libsodium.org/>`_.  libsodium is fetched automatically via
CMake FetchContent at configure time; no manual installation is required.

Server Setup
------------

Generating a Server Key
~~~~~~~~~~~~~~~~~~~~~~~

The server needs a long-term X25519 identity keypair.  Generate one with
``GenerateServerSecurityKey`` (declared in ``mafianet/crypto/keys.h``):

.. code-block:: cpp

   #include "mafianet/crypto/keys.h"

   MafiaNet::ServerSecurityKey key = MafiaNet::GenerateServerSecurityKey();

   // Persist the keypair — the secret key must stay on the server.
   // The public key (32 bytes) is distributed to all clients.
   SaveToFile("server.key", key.secretKey, 32);
   SaveToFile("server.pub", key.publicKey, 32);

``ServerSecurityKey`` is a plain struct:

.. code-block:: cpp

   struct ServerSecurityKey {
       unsigned char publicKey[32];  // X25519 public key  — distribute to clients
       unsigned char secretKey[32];  // X25519 secret key  — keep on server only
   };

Installing the Key on the Server
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Call ``SetServerSecurityKey`` **before** ``Startup``.  A server with no key
installed **refuses every incoming connection** (it has no way to encrypt the
link and fails closed rather than falling back to plaintext), so the key is
mandatory to accept connections.

.. code-block:: cpp

   #include "mafianet/peerinterface.h"
   #include "mafianet/crypto/keys.h"

   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   // Load or generate the long-term identity keypair.
   MafiaNet::ServerSecurityKey key;
   LoadFromFile("server.key", key.secretKey, 32);
   LoadFromFile("server.pub", key.publicKey, 32);

   peer->SetServerSecurityKey(key);

   // Start listening.
   MafiaNet::SocketDescriptor sd(60000, 0);
   peer->Startup(100, &sd, 1);
   peer->SetMaximumIncomingConnections(100);

Client Setup
------------

The client must supply the server's 32-byte public key when calling
``Connect``.  This is the **pinned** key — connections to any host that does
not possess the matching secret key will fail.

.. code-block:: cpp

   #include "mafianet/peerinterface.h"

   // Embed or load the server's public key (32 bytes).
   unsigned char serverPublicKey[32];
   LoadFromFile("server.pub", serverPublicKey, 32);

   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   MafiaNet::SocketDescriptor sd;
   peer->Startup(1, &sd, 1);

   peer->Connect(
       "game.example.com",  // host
       60000,               // port
       nullptr,             // password data (optional)
       0,                   // password length
       serverPublicKey      // REQUIRED: pinned server public key (32 bytes)
   );

Key Distribution
~~~~~~~~~~~~~~~~

* **Never distribute the secret key.**  It never leaves the server.
* Embed the 32-byte public key in the client binary at build time (common
  approach), or load it from a file shipped with the client.
* Rotate keys by regenerating a new keypair, redeploying the server binary,
  and shipping an updated client with the new public key embedded.

FullyConnectedMesh2 and Auto-Connections
-----------------------------------------

``FullyConnectedMesh2::SetConnectOnNewRemoteConnection`` now accepts an optional
third argument — the pinned server public key for auto-mesh connections:

.. code-block:: cpp

   // All peers in the mesh share the same pinned server key.
   fcm2.SetConnectOnNewRemoteConnection(true, "gamePassword", sharedServerPublicKey);

If the key pointer is null (the default), ``SetConnectOnNewRemoteConnection``
will **not** auto-connect to new peers — the mesh is fail-closed without a key.

Threat Model and Non-Goals
--------------------------

**What MafiaNet's default encryption provides:**

* Confidentiality — traffic is unreadable to passive observers.
* Integrity — tampered datagrams are rejected.
* Forward secrecy — compromising the server's long-term key does not expose
  past session traffic (ephemeral keys are used for each session).
* Server authentication — clients verify the server's identity via the pinned
  public key; a man-in-the-middle cannot impersonate the server.
* Replay protection — a 64-entry sliding-window filter rejects replayed
  datagrams.

**Explicit non-goals:**

* **Client authentication** — the client is anonymous; the server cannot verify
  the client's identity at the transport layer.  Use the
  :doc:`../plugins/two-way-authentication` plugin or application-level tokens
  for client auth.
* **Post-quantum security** — X25519 is not quantum-resistant.
* **DoS resistance beyond return-routability** — the HMAC cookie prevents
  spoofed-source amplification attacks, but a volumetric flood can still
  exhaust the server.
* **Certificate revocation** — there is no CRL or OCSP.  Key rotation requires
  a client update.

Handling Security Events
------------------------

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_PUBLIC_KEY_MISMATCH:
           // Handshake failed: server key does not match the pinned key.
           // Possible MITM or wrong server — do not proceed.
           printf("Server key mismatch — possible MITM attack!\n");
           break;

       case ID_CONNECTION_ATTEMPT_FAILED:
           // Could also indicate a handshake failure.
           printf("Connection failed\n");
           break;
   }

Best Practices
--------------

1. **Never ship the secret key to clients.** Keep it on the server only.
2. **Embed the public key at build time** — do not download it at runtime (that
   would defeat pinning).
3. **Rotate keys for major server updates** — generate a new keypair, redeploy
   the server, ship an updated client.
4. **Still validate application-layer data** — transport encryption prevents
   eavesdropping but not malicious payloads from a legitimately-connected
   client.
5. **Use TwoWayAuthentication for client identity** — if you need to verify
   *who* the client is, layer the :doc:`../plugins/two-way-authentication`
   plugin on top.

.. _secure-connections-migration:

Migration from 0.7.x
---------------------

The following API was **removed** in 0.8.0:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Removed (0.7.x)
     - Replacement (0.8.0)
   * - ``RakPeerInterface::InitializeSecurity``
     - ``RakPeerInterface::SetServerSecurityKey``
   * - ``RakPeerInterface::DisableSecurity``
     - No opt-out — encryption is always on
   * - ``RakPeerInterface::AddToSecurityExceptionList``
     - Removed — no exception list concept
   * - ``RakPeerInterface::RemoveFromSecurityExceptionList``
     - Removed
   * - ``RakPeerInterface::GetClientPublicKeyFromSystemAddress``
     - Removed — client is anonymous
   * - ``PublicKey`` struct / ``PublicKeyMode`` enum
     - Pass ``const unsigned char serverPublicKey[32]`` directly to ``Connect``
   * - ``Connect(..., PublicKey*)``
     - ``Connect(..., const unsigned char serverPublicKey[32])``
   * - libcat / ``LIBCAT_SECURITY``
     - libsodium (fetched automatically)
   * - ``GenerateServerRSAKeys``
     - ``GenerateServerSecurityKey`` (returns ``ServerSecurityKey``)

**Wire compatibility:** peers built against 0.8.0 cannot talk to 0.7.x peers.
Plan a coordinated server+client upgrade.

See Also
--------

* :doc:`connecting` - Connection basics
* :doc:`../plugins/two-way-authentication` - Application-level client authentication
* :doc:`../plugins/fully-connected-mesh-2` - P2P mesh with auto-connections
