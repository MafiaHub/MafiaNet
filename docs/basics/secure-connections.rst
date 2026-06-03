Secure Connections
==================

As of an upcoming release, **all connections are encrypted and authenticated by
default** using the `Noise_NK`_ protocol pattern built on **libsodium**.  There
is no opt-out switch: every peer-to-peer datagram is encrypted, integrity-
protected, and replay-guarded from the moment the handshake completes.

.. _Noise_NK: https://noiseprotocol.org/

.. note::

   **Breaking change from the previous release.** The wire protocol is incompatible with
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

The server needs a long-term X25519 identity keypair.  ``ServerSecurityKey``
is a plain struct (declared in ``mafianet/crypto/keys.h``):

.. code-block:: cpp

   struct ServerSecurityKey {
       unsigned char publicKey[32];  // X25519 public key  — distribute to clients
       unsigned char secretKey[32];  // X25519 secret key  — keep on server only
   };

**Only the secret key needs to be stored.**  The public key is mathematically
derived from the secret, so you can always reconstruct the full keypair from
the secret alone.  No key *file* is required — a string in a config file or
environment variable is sufficient.

The helpers below are declared in ``mafianet/crypto/keys.h`` and use libsodium
internally.  Buffer-size constants are provided for convenience:

.. code-block:: cpp

   static const size_t SERVER_KEY_HEX_LEN    = 65;  // 64 hex chars + NUL
   static const size_t SERVER_KEY_BASE64_LEN = 45;  // base64 chars + NUL

**Option 1 — One-off generation (key-generation tool)**

Run this once, print the strings, then paste them into your config.  Only the
secret line needs to be kept secret; the public line is safe to distribute.

.. code-block:: cpp

   #include "mafianet/crypto/keys.h"

   MafiaNet::ServerSecurityKey k = MafiaNet::GenerateServerSecurityKey();

   char secHex[MafiaNet::SERVER_KEY_HEX_LEN];
   char pubHex[MafiaNet::SERVER_KEY_HEX_LEN];
   MafiaNet::ServerSecurityKeySecretToHex(k, secHex, sizeof secHex);
   MafiaNet::ServerSecurityKeyPublicToHex(k, pubHex, sizeof pubHex);
   printf("server_secret = %s\nserver_public = %s\n", secHex, pubHex);

   // Base64 variants are also available:
   char secB64[MafiaNet::SERVER_KEY_BASE64_LEN];
   char pubB64[MafiaNet::SERVER_KEY_BASE64_LEN];
   MafiaNet::ServerSecurityKeySecretToBase64(k, secB64, sizeof secB64);
   MafiaNet::ServerSecurityKeyPublicToBase64(k, pubB64, sizeof pubB64);

**Option 2 — Load from a config string (recommended; no file required)**

Store only the secret string in your server config.  At startup, pass it to
``ServerSecurityKeyFromSecretHex`` (or ``ServerSecurityKeyFromSecretBase64``
for a base64-encoded value); the public key is derived automatically.

.. code-block:: cpp

   #include "mafianet/crypto/keys.h"

   MafiaNet::ServerSecurityKey key;
   const char* secret = config.get("server_secret");  // hex string from config
   if (!MafiaNet::ServerSecurityKeyFromSecretHex(secret, key)) {
       // Handle bad or missing config value.
       fprintf(stderr, "Invalid server_secret in config\n");
       return -1;
   }
   peer->SetServerSecurityKey(key);

   // Base64 variant:
   // if (!MafiaNet::ServerSecurityKeyFromSecretBase64(secret, key)) { ... }

**Option 3 — Environment variable**

Same as Option 2, but source the secret from an environment variable instead
of a config file — useful for container deployments.

.. code-block:: cpp

   #include "mafianet/crypto/keys.h"
   #include <cstdlib>

   MafiaNet::ServerSecurityKey key;
   const char* secret = std::getenv("MAFIANET_SERVER_SECRET");
   if (!secret || !MafiaNet::ServerSecurityKeyFromSecretHex(secret, key)) {
       fprintf(stderr, "MAFIANET_SERVER_SECRET missing or invalid\n");
       return -1;
   }
   peer->SetServerSecurityKey(key);

**Option 4 — Ephemeral key (local testing only)**

Generate a fresh keypair on every startup.  This is the simplest option, but
clients cannot pin a key that changes every restart, so it is only suitable
for local development and automated tests.

.. code-block:: cpp

   MafiaNet::ServerSecurityKey key = MafiaNet::GenerateServerSecurityKey();
   peer->SetServerSecurityKey(key);

**Option 5 — Binary file**

Write and read the raw 32-byte secret (or full keypair) to/from a file.

.. code-block:: cpp

   // Generate and save (run once).
   MafiaNet::ServerSecurityKey key = MafiaNet::GenerateServerSecurityKey();
   SaveToFile("server.key", key.secretKey, 32);

   // Load at startup (derive the public from the stored secret).
   unsigned char sec[32];
   LoadFromFile("server.key", sec, 32);
   MafiaNet::ServerSecurityKey key = MafiaNet::ServerSecurityKeyFromSecret(sec);

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

   // Load the long-term identity keypair from a config string.
   // Only the secret needs to be stored; the public is derived automatically.
   MafiaNet::ServerSecurityKey key;
   if (!MafiaNet::ServerSecurityKeyFromSecretHex(config.get("server_secret"), key)) {
       fprintf(stderr, "Invalid server_secret\n");
       return -1;
   }

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

**Loading the pinned public key from a config string (recommended)**

Distribute only the public key string to clients (never the secret).  Parse it
at startup with ``ServerPublicKeyFromHex`` or ``ServerPublicKeyFromBase64``:

.. code-block:: cpp

   #include "mafianet/crypto/keys.h"

   unsigned char serverPublicKey[32];
   if (!MafiaNet::ServerPublicKeyFromHex(config.get("server_public"), serverPublicKey)) {
       fprintf(stderr, "Invalid server_public in config\n");
       return -1;
   }
   // Then pass serverPublicKey to Connect (see below).

   // Base64 variant:
   // if (!MafiaNet::ServerPublicKeyFromBase64(config.get("server_public"), serverPublicKey)) { ... }

**Connecting with the pinned key**

.. code-block:: cpp

   #include "mafianet/peerinterface.h"
   #include "mafianet/crypto/keys.h"

   // Embed or load the server's public key (32 bytes) — see above.
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
* Ship clients the **public key string** (hex or base64) embedded in the
  binary, a config file, or a file shipped with the client.  Parse it at
  startup with ``ServerPublicKeyFromHex`` / ``ServerPublicKeyFromBase64``.
* Rotate keys by regenerating a new keypair (Option 1 above), redeploying
  the server with the new secret, and shipping an updated client with the
  new public key.

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

Migration from Prior Releases
------------------------------

The following API was **removed** in the next release:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Removed (previous release)
     - Replacement (next release)
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

**Wire compatibility:** the new release peers cannot talk to peers built against the previous release.
Plan a coordinated server+client upgrade.

See Also
--------

* :doc:`connecting` - Connection basics
* :doc:`../plugins/two-way-authentication` - Application-level client authentication
* :doc:`../plugins/fully-connected-mesh-2` - P2P mesh with auto-connections
