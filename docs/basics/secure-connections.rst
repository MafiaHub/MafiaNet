Secure Connections
==================

Every MafiaNet connection is encrypted and authenticated per datagram by default. There is nothing
to enable, no key to generate and no build flag to set: two default peers connect encrypted.

.. code-block:: cpp

   // Server and client, unchanged from an unencrypted program. Both are encrypted.
   MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();
   MafiaNet::SocketDescriptor sd(60000, 0);
   server->Startup(100, &sd, 1);
   server->SetMaximumIncomingConnections(100);

   MafiaNet::RakPeerInterface* client = MafiaNet::RakPeerInterface::GetInstance();
   MafiaNet::SocketDescriptor csd;
   client->Startup(1, &csd, 1);
   client->Connect("game.example.com", 60000, nullptr, 0);

This page says what that protects and what it does not. The byte-level description is in
:doc:`../advanced/encryption-wire-format`.

How it works
------------

During the connection handshake each side generates a fresh ephemeral X25519 key pair and sends its
public key. Both sides compute the same shared secret, derive two directional keys from it with
HKDF-SHA256, and wipe the ephemeral secret key. From then on every datagram of that connection is
sealed with ChaCha20-Poly1305 under a per-direction key and a 64-bit sequence number. All crypto
comes from `libsodium <https://libsodium.org>`_ (1.0.18 or later), which is the only crypto
dependency; OpenSSL is no longer used.

What is protected
-----------------

* **Confidentiality.** The contents of every connected datagram are encrypted: your messages,
  the connection password, the session configuration payload (see
  :doc:`../advanced/session-handshake-security`), and all plugin traffic that runs over a connection.
* **Integrity and authenticity of the datagram.** Every datagram carries a 16-byte Poly1305 tag. A
  datagram that was modified, forged, or sent by someone who does not hold the session keys fails
  the tag and is dropped.
* **Replay protection.** Each datagram has a sequence number that is part of the nonce. A receiver
  keeps a 256-entry window and drops any sequence it has already seen, and any sequence older than
  the window.
* **Forward secrecy.** The session keys come from ephemeral keys that exist only for the handshake
  and are wiped as soon as the keys are derived. There is no long-term key, so recording a
  connection and later compromising a machine does not reveal it.
* **No effect on the connection from junk.** A datagram that fails to authenticate is dropped and
  counted. It never disconnects the peer, never asserts, and never reaches plugins. See
  `Monitoring dropped datagrams`_.

What is not protected
---------------------

* **Nobody is authenticated.** MafiaNet encrypts the connection but does not yet verify who is on
  the other end. A client cannot tell the real server from an attacker who sits on the path
  between them and relays the handshake, and a server cannot tell clients apart this way. An
  on-path attacker can therefore read and change the traffic of a connection it intercepts from the
  first handshake packet. The encryption defeats passive eavesdropping and off-path injection; it
  does not defeat an active man in the middle. Server identity keys are planned (the reserved
  ``PublicKey*`` parameter of ``Connect`` is where a pin will go) and will bump the protocol
  version again. Until then, authenticate users with an application-level mechanism such as
  :doc:`../plugins/two-way-authentication`, and do not treat "connected" as "trusted".
* **Offline messages.** Anything sent outside a connection is plaintext: pings and pongs,
  ``ID_ADVERTISE_SYSTEM``, unconnected sends, NAT punchthrough traffic, and the open-connection
  handshake packets themselves.
* **Packet sizes and timing.** Datagram lengths (the payload length plus a fixed overhead), the
  moments datagrams are sent, and their count are visible to an observer. Nothing is padded.
* **Denial of service during connect.** An attacker who knows a connecting client's address and
  port, and who can send packets to it while the connection attempt is in progress, can forge one of
  the refusal IDs below and make that attempt fail. It cannot learn or choose the session keys. This
  is the same property the plaintext rejection IDs (``ID_CONNECTION_BANNED`` and others) always had.
  Forging the server's final handshake reply also takes a 64-bit value the client chose, which only
  an attacker on the path sees.
* **Endpoints.** Encryption protects the wire, not a compromised client or server, and not a
  malicious but connected peer: validate all input as before.

Choosing the encryption mode
----------------------------

.. code-block:: cpp

   enum class EncryptionMode : uint8_t { Required = 0, Plaintext = 1 };

``EncryptionMode::Required`` is the default. ``EncryptionMode::Plaintext`` sends connected
datagrams in the clear and exists for LAN debugging and fuzzing. Call ``SetEncryptionMode`` before
``Startup``; it returns ``false`` and changes nothing once the peer has started.

.. code-block:: cpp

   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   if (!peer->SetEncryptionMode(MafiaNet::EncryptionMode::Plaintext)) {
       // Already started: call SetEncryptionMode before Startup().
   }
   peer->Startup(100, &sd, 1);

   MafiaNet::EncryptionMode mode = peer->GetEncryptionMode();

The startup builders take the same setting:

.. code-block:: cpp

   auto server = MafiaNet::Peer::server()
       .port(60000)
       .max_connections(32)
       .encryption(MafiaNet::EncryptionMode::Plaintext)   // omit for the default, Required
       .start();

   auto client = MafiaNet::Peer::client()
       .max_connections(1)
       .encryption(MafiaNet::EncryptionMode::Plaintext)
       .connect("127.0.0.1", 60000)
       .start();

A failed ``SetEncryptionMode`` surfaces as ``PeerStage::Startup`` in the builder's error.

**Both peers must use the same mode.** There is no negotiation and no fallback: a mismatch refuses
the connection rather than quietly downgrading it. A plaintext peer never connects to an encrypted
one by accident.

Handling a mode mismatch
~~~~~~~~~~~~~~~~~~~~~~~~

The refusal is reported to the side that called ``Connect``. The accepting side's application sees
nothing, and the server allocates no connection state.

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Client
     - Server
     - Client receives
   * - ``Plaintext``
     - ``Required``
     - ``ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION``
   * - ``Required``
     - ``Plaintext``
     - ``ID_OUR_SYSTEM_REQUIRES_SECURITY``

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION:
           printf("The server requires encryption; this client is plaintext\n");
           break;

       case ID_OUR_SYSTEM_REQUIRES_SECURITY:
           printf("This client requires encryption; the server is plaintext\n");
           break;
   }

``ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION`` is the new name of ``ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY``.
The old name still compiles as a ``[[deprecated]]`` alias with the same numeric value, so existing
switches keep working; rename it when you can (the deprecation warning is an error under
``-Werror``). ``ID_PUBLIC_KEY_MISMATCH`` is reserved for the future identity keys and is never
sent today.

The reserved public key parameter
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``Connect`` and ``ConnectWithSocket`` keep their ``PublicKey *publicKey`` parameter so that call
sites with later arguments keep their meaning. It is reserved for server identity keys: pass
``nullptr`` (the default). A non-null value returns ``INVALID_PARAMETER``.

Migrating from the old security API
-----------------------------------

The RSA and ``libcat`` based API, which was optional and compiled in only on request, is gone. Delete
these calls; encryption now needs none of them:

* ``InitializeSecurity``, ``DisableSecurity``, ``AddToSecurityExceptionList``,
  ``RemoveFromSecurityExceptionList``, ``IsInSecurityExceptionList``
* ``GenerateServerRSAKeys`` and the key files
* ``GetClientPublicKeyFromSystemAddress``
* ``PublicKeyMode`` and the ``PKM_*`` values
* ``ServerBuilder::secure()`` and ``ClientBuilder::public_key()``
* the ``LIBCAT_SECURITY`` build flag

Removed along with OpenSSL: TLS in ``TCPInterface`` (``StartSSLClient``, ``IsSSLActive``), HTTPS in
``HTTPConnection`` and ``HTTPConnection2`` (``TransmitRequest`` lost its ``useSSL`` parameter),
SMTPS and authentication in ``EmailSender`` (plaintext SMTP only; the password argument is
ignored), the experimental ``crypto/`` file encrypter, and the Rackspace plugin.

The protocol version is now 9. A peer built before this change answers the first handshake packet
with ``ID_INCOMPATIBLE_PROTOCOL_VERSION``.

Simultaneous connects
---------------------

When two peers call ``Connect`` to each other at the same moment (a cross-connection, common in
peer-to-peer meshes), each would otherwise derive keys from a different handshake. MafiaNet resolves
this with a fixed rule: **the handshake started by the peer with the lower** ``RakNetGUID`` **wins**.

* The lower peer does not answer the higher peer's handshake while its own attempt is still
  pending.
* The higher peer answers the lower peer's handshake normally. Its own ``Connect`` is absorbed into
  that incoming connection.

What your code sees: the lower peer gets ``ID_CONNECTION_REQUEST_ACCEPTED``. The higher peer gets
``ID_NEW_INCOMING_CONNECTION`` and never ``ID_CONNECTION_REQUEST_ACCEPTED``, even though it called
``Connect``. If the winning connection dies before it completes, the higher peer gets
``ID_CONNECTION_ATTEMPT_FAILED`` for its ``Connect``. Code that waits specifically for
``ID_CONNECTION_REQUEST_ACCEPTED`` after calling ``Connect`` in a peer-to-peer setup must also
handle ``ID_NEW_INCOMING_CONNECTION``. Plaintext peers behave as they always did.

Monitoring dropped datagrams
----------------------------

``RakNetStatistics::droppedUnauthenticatedDatagrams`` counts datagrams received on a connection
that failed to authenticate: forged, corrupted, replayed, or too old for the replay window. It is
always ``0`` on a plaintext connection.

.. code-block:: cpp

   MafiaNet::RakNetStatistics stats;
   if (peer->GetStatistics(addr, &stats)) {
       if (stats.droppedUnauthenticatedDatagrams > 0) {
           printf("%llu datagrams failed authentication\n",
                  (unsigned long long) stats.droppedUnauthenticatedDatagrams);
       }
   }

A small number can be normal: the network can duplicate a datagram, and a duplicate is dropped as a
replay. A count that keeps rising points at an attacker injecting traffic at that address. The
connection itself is unaffected.

Cost
----

* **Bandwidth.** Each connected datagram grows by up to 25 bytes: 1 prefix byte, 1 to 8 bytes of
  sequence number, and a 16-byte tag. MafiaNet reserves the
  full 25 bytes out of the MTU budget, so messages are split exactly as before and sealed datagrams
  still fit the path MTU. The effective payload per datagram is 25 bytes smaller in
  ``Required`` mode, nothing changes in ``Plaintext`` mode.
* **Handshake.** One X25519 scalar multiplication per side, plus a stateless challenge check on the
  server that costs one SipHash. No extra round trip is added.
* **Throughput.** ChaCha20-Poly1305 is fast without hardware support. ``Benchmarks/`` has
  microbenchmarks for sealing, opening and key derivation.

Best practices
--------------

1. **Leave encryption on.** Use ``Plaintext`` only on a trusted LAN, in tests, or under a fuzzer.
2. **Do not rely on encryption for identity.** Until server identity keys exist, authenticate users
   at the application level and assume an active attacker can sit between a client and the server.
3. **Validate all data.** Encryption does not make a peer's messages correct or benign.
4. **Keep libsodium current.** It is the one crypto dependency; update it with your OS or package
   manager.

TwoWayAuthentication plugin
---------------------------

For password-based authentication without exposing the password, see
:doc:`../plugins/two-way-authentication`. It authenticates a user to the server and is independent
of the transport encryption.

See Also
--------

* :doc:`connecting` - Connection basics
* :doc:`../advanced/encryption-wire-format` - Byte-level protocol description
* :doc:`../advanced/session-handshake-security` - Trust model of the session payload
* :doc:`../plugins/two-way-authentication` - Password authentication
