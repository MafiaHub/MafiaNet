Secure Connections
==================

Every MafiaNet connection is encrypted and integrity-protected per datagram by default. There is nothing
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
  and are wiped as soon as the keys are derived. A server identity key (see `Server identity`_) only
  signs the handshake and never encrypts anything, so recording a connection and later compromising a
  machine does not reveal it.
* **The server's identity, when the client pins it.** A client given the server's identity public
  key accepts only a server that proves it holds the matching secret key. See `Server identity`_.
* **No effect on the connection from junk.** A datagram that fails to authenticate is dropped and
  counted. It never disconnects the peer, never asserts, and never reaches plugins. See
  `Monitoring dropped datagrams`_.

What is not protected
---------------------

* **The server, unless the client pins it.** Without a pinned server identity, a client cannot tell
  the real server from an attacker who sits on the path between them and relays the handshake. The
  encryption then defeats passive eavesdropping and off-path injection, but not an active man in
  the middle. Pin the server identity (see `Server identity`_) wherever the client can be given the
  server's public key.
* **Clients.** Nothing authenticates a client to a server. Authenticate users with an
  application-level mechanism such as :doc:`../plugins/two-way-authentication`, and do not treat
  "connected" as "trusted".
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
  an attacker on the path sees. An attacker on the path can likewise make a pinned attempt end with
  ``ID_PUBLIC_KEY_MISMATCH``, but cannot make it succeed.
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
``-Werror``). ``ID_PUBLIC_KEY_MISMATCH`` keeps its value and now reports a pinned server identity
that was not proven (see `Server identity`_).

.. _server-identity:

Server identity
---------------

A server can hold a long-term **identity key**, an Ed25519 key pair. With one, the server signs every
connection handshake it answers. A client that was given the server's public key ("pins" it) checks
that signature and refuses any server that cannot produce it, so an attacker on the path can no
longer stand in for the server.

Generate a key pair
~~~~~~~~~~~~~~~~~~~

Once per server, with ``GenerateIdentityKeyPair`` from ``mafianet/session_crypto.h`` (also reachable
as ``RakPeerInterface::GenerateIdentityKeyPair``), or with the ``IdentityKeyGen`` sample, which prints
a new pair in hex:

.. code-block:: cpp

   #include "mafianet/session_crypto.h"

   uint8_t publicKey[MafiaNet::kIdentityPublicKeyBytes];   // 32 bytes: publish
   uint8_t secretKey[MafiaNet::kIdentitySecretKeyBytes];   // 64 bytes: keep private
   MafiaNet::GenerateIdentityKeyPair(publicKey, secretKey);

Store the secret key the way you store other server secrets. Ship the public key with the client, or
publish it through any channel the client already trusts.

There is no revocation. If the secret key is compromised, generate a new pair, install it on the
server and ship the new public key to the clients; clients that still pin the old key keep trusting
whoever holds it until they are updated.

Install it on the server
~~~~~~~~~~~~~~~~~~~~~~~~

``SetIdentityKey`` must be called before ``Startup``. It returns ``false`` once the peer has started,
and for a malformed key.

.. code-block:: cpp

   MafiaNet::RakPeerInterface* server = MafiaNet::RakPeerInterface::GetInstance();
   if (!server->SetIdentityKey(secretKey)) {
       // Already started, or not a valid 64-byte Ed25519 secret key.
   }
   server->Startup(100, &sd, 1);
   MafiaNet::SecureZero(secretKey, sizeof(secretKey));   // the peer keeps its own copy

   uint8_t check[32];
   server->GetIdentityPublicKey(check);   // the public half, false if no key is set

The peer keeps its own copy of the key across ``Shutdown`` and ``Startup``, like the encryption mode
and the incoming password, so a restarted server keeps signing. The copy is wiped when it is replaced,
when ``SetIdentityKey(nullptr)`` removes it (both only while the peer is stopped), and when the peer
is destroyed. A server without an identity key behaves as before.

Pin it on the client
~~~~~~~~~~~~~~~~~~~~

Pass the server's public key to ``Connect`` (or ``ConnectWithSocket``) in a ``PublicKey``:

.. code-block:: cpp

   MafiaNet::PublicKey pin;
   memcpy(pin.serverIdentity, serverPublicKey, sizeof(pin.serverIdentity));
   client->Connect("game.example.com", 60000, nullptr, 0, &pin);

   switch (packet->data[0]) {
       case ID_CONNECTION_REQUEST_ACCEPTED:
           // The server proved it holds the pinned key.
           break;
       case ID_PUBLIC_KEY_MISMATCH:
           // It did not: no key, another key, or a bad signature. The attempt is over.
           break;
   }

``ID_PUBLIC_KEY_MISMATCH`` arrives once per ``Connect``, under the address ``Connect`` reported, even
when several addresses of the host were tried (IPv6 and IPv4). Nothing was allocated for the attempt.

The startup builders take the same settings:

.. code-block:: cpp

   auto server = MafiaNet::Peer::server()
       .port(60000)
       .max_connections(32)
       .identity_key(secretKey)            // SetIdentityKey before Startup
       .start();

   auto client = MafiaNet::Peer::client()
       .pin_server_identity(serverPublicKey)
       .connect("game.example.com", 60000)
       .start();

Without a pin
~~~~~~~~~~~~~

A client that pinned nothing still checks the signature of a server that sends one. If it verifies,
the client records the server's public key, readable once connected:

.. code-block:: cpp

   uint8_t key[32];
   if (client->GetRemoteIdentityKey(serverAddress, key)) {
       // The server proved it holds this key. Whether it is the server you meant is for
       // the application to decide, for example by remembering it on first use.
   }

``GetRemoteIdentityKey`` is valid once ``ID_CONNECTION_REQUEST_ACCEPTED`` has arrived, and returns
``false`` when the server has no identity key. If the signature does not verify, the client ignores
that handshake reply and keeps waiting for a valid one; it does not report
``ID_PUBLIC_KEY_MISMATCH``, which only a pinned attempt does.

What the signature covers
~~~~~~~~~~~~~~~~~~~~~~~~~

The server signs the two crypt infos of the handshake (see
:doc:`../advanced/encryption-wire-format`). It does not sign the server's ``RakNetGUID`` or the MTU
in that reply. In ``Required`` mode the GUIDs still enter the session keys, so the identity extends to
the whole encrypted connection.

Identity works in both encryption modes, but in ``Plaintext`` mode it only proves that the real server
answered the handshake. After the handshake nothing is protected: an attacker on the path can still
read and change everything, including the session configuration payload, and the GUID and MTU of the
reply can be changed. A pin does not make a ``Plaintext`` connection trustworthy.

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
``HTTPConnection`` and ``HTTPConnection2`` (``TransmitRequest`` lost its ``useSSL`` parameter, and
a call that still passes a ``bool`` there no longer compiles),
SMTPS and authentication in ``EmailSender`` (plaintext SMTP only; the password argument is
ignored), the experimental ``crypto/`` file encrypter, and the Rackspace plugin.

Protocol 9 introduced this handshake. The current protocol is 10. A peer using a different version answers the first handshake packet
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

With pinned identities, only the side that answers a handshake proves its identity:

* The lower peer pins the higher one: the higher peer answers as above and proves its key, so it
  needs an identity key.
* The higher peer pins the lower one: its ``Connect`` is never absorbed into the lower peer's
  handshake. It refuses that handshake with ``ID_ALREADY_CONNECTED``, which the lower peer receives
  for its own ``Connect``, and connects as the initiator; the lower peer then answers and must prove
  the pinned key. The higher peer gets ``ID_CONNECTION_REQUEST_ACCEPTED`` and the lower one
  ``ID_NEW_INCOMING_CONNECTION``.
* Both pin each other: as in the previous case. Only the higher peer's pin is enforced; the lower
  peer's ``Connect`` ended with ``ID_ALREADY_CONNECTED`` and the higher peer proved nothing to it.

**Trust in peer-to-peer.** An incoming connection is never authenticated: the remote that initiated
it proved no identity. So in a peer-to-peer setup, decide whether to trust a remote by
``GetRemoteIdentityKey(address)`` returning the key you expect, not by having called ``Connect`` with
a pin. When both sides pin each other, only one side's pin is enforced, as above. A pinned
``Connect`` that returns ``ALREADY_CONNECTED_TO_ENDPOINT`` (the remote's handshake already won the
race) checked nothing, and a pinned attempt that ended in ``ID_PUBLIC_KEY_MISMATCH`` or
``ID_ALREADY_CONNECTED`` can be followed by an incoming connection from the same address, which is
just as unauthenticated.

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
2. **Pin the server identity.** Give servers an identity key and have clients pin it. Without a
   pin, assume an active attacker can sit between a client and the server. Authenticate users at the
   application level either way.
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
