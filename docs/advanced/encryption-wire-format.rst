Encryption Wire Format
======================

This page specifies how MafiaNet protocol version 9 establishes and uses session keys, precisely
enough to write an interoperable implementation. For what the encryption does and does not protect,
and how to use it, see :doc:`../basics/secure-connections`.

It is written against the implementation in ``session_crypto.cpp``, ``handshake_challenge.cpp`` and
the open-connection handlers in ``peer.cpp``. Where this page and the code differ, the code is right.

Conventions
-----------

* All primitives come from libsodium: ``crypto_scalarmult_curve25519`` (X25519),
  ``crypto_auth_hmacsha256``, ``crypto_aead_chacha20poly1305_ietf`` (ChaCha20-Poly1305, 12-byte
  nonce, 16-byte tag) and ``crypto_shorthash`` (SipHash-2-4).
* Fields written by ``BitStream`` (the message ID, GUIDs, the challenge, the MTU) are byte-aligned
  here, and multi-byte integers are in **network byte order (big-endian)**.
* The fields described as "little-endian" below (the crypt info nonce, the sequence number, the
  nonce, and the integers fed to the key schedule) are written byte by byte by the crypto code and
  are little-endian on every platform.
* A ``SystemAddress`` is ``[u8 4][4 address bytes, each bitwise inverted][u16 port, network order]``
  for IPv4 (7 bytes), or ``[u8 6][16 address bytes, each inverted][u16 port]`` for IPv6 (19 bytes).
  An IPv4-mapped IPv6 address is sent in the IPv4 form.
* ``OFFLINE_ID`` is the 16-byte offline message marker
  ``00 FF FF 00 FE FE FE FE FD FD FD FD 12 34 56 78``.
* The protocol version, ``RAKNET_PROTOCOL_VERSION``, is **9**. It occupies one byte.
* ``initiator`` is the peer that called ``Connect``; ``responder`` is the peer that accepted. In a
  simultaneous connect the winner of the tie-break (see `Simultaneous connect`_) is the initiator.

Overview
--------

The connection handshake keeps its four offline messages and adds two things to them: a stateless
challenge (so the responder does no work and allocates nothing for an address that cannot receive
packets), and a *crypt info* record from each side.

::

    Client                                    Server
      |  OPEN_CONNECTION_REQUEST_1  ---------->|   protocol version, MTU probe
      |<---------- OPEN_CONNECTION_REPLY_1     |   server GUID, challenge, MTU
      |  OPEN_CONNECTION_REQUEST_2  ---------->|   challenge, client crypt info, ...
      |<---------- OPEN_CONNECTION_REPLY_2     |   server crypt info, ...
      |                                        |
      |  both sides now derive the same two session keys
      |  ID_CONNECTION_REQUEST (sealed)  ----->|   first sealed datagram
      |<----- everything else, sealed ------->|

There is no explicit key confirmation message. As in GameNetworkingSockets, the first sealed
datagram, ``ID_CONNECTION_REQUEST``, confirms the keys: if the two sides derived different keys it
cannot be opened and the connection never forms.

Handshake messages
------------------

These four messages, and the refusal messages, are offline messages: **they are not encrypted**.
Anything in them can be read or replaced by anyone on the path.

ID_OPEN_CONNECTION_REQUEST_1 (client to server)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

    [message ID] (u8)
    [OFFLINE_ID] (16 bytes)
    [protocol version] (u8) // 9
    <zero pad up to the MTU being probed>

**At least 35 bytes**, the size of ``REPLY_1``: a server silently ignores a shorter ``REQUEST_1``,
so answering one never sends back more than it received. A real client always pads to the MTU it
is probing, at least 548 bytes (the smallest rung of the MTU ladder, 576, minus 28 bytes of IP and
UDP headers).

A server whose version differs answers with ``ID_INCOMPATIBLE_PROTOCOL_VERSION`` (26 bytes) and does
nothing else. The padding makes the datagram as large as the MTU being probed; the server's MTU in
the reply follows from the datagram's size.

ID_OPEN_CONNECTION_REPLY_1 (server to client)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

    [message ID] (u8)
    [OFFLINE_ID] (16 bytes)
    [server GUID] (u64)
    [challenge] (u64)       // see "The challenge"
    [MTU] (u16)

35 bytes. The server stores nothing: the challenge is recomputed when it is echoed back.

ID_OPEN_CONNECTION_REQUEST_2 (client to server)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

    [message ID] (u8)
    [OFFLINE_ID] (16 bytes)
    [challenge] (u64)       // echoed from REPLY_1, unchanged
    [client crypt info] (41 bytes)
    [binding address] (SystemAddress) // the server's address, as the client sees it
    [MTU] (u16)
    [client GUID] (u64)

83 bytes with an IPv4 binding address, 95 with IPv6.

The server checks the challenge first, before reading anything else. If it fails, the datagram is
dropped without a reply and nothing is allocated or computed. Only then are the remaining fields
read; a short read drops the datagram.

ID_OPEN_CONNECTION_REPLY_2 (server to client)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

    [message ID] (u8)
    [OFFLINE_ID] (16 bytes)
    [nonce echo] (u64)      // the nonce of the client crypt info this answers, see below
    [server GUID] (u64)
    [client address] (SystemAddress) // the client's address, as the server sees it
    [MTU] (u16)
    [server crypt info] (41 bytes)

83 bytes with an IPv4 client address, 95 with IPv6.

The nonce echo is the ``nonce`` field of the client crypt info from the ``REQUEST_2`` being answered,
written by ``BitStream`` (network byte order, like the challenge). It plays the part of the client's
``connection_id`` that GameNetworkingSockets echoes in its ConnectOK. Before anything else, the
client compares it with the nonce of its pending attempt to that address and silently drops a
``REPLY_2`` that does not match, so an off-path attacker has to guess 64 random bits to get a forged
``REPLY_2`` processed at all.

Crypt info
~~~~~~~~~~

Both sides send the same 41-byte record::

    [mode] (u8)              // 0 = Required, 1 = Plaintext; any other value is malformed
    [X25519 public key] (32 bytes)
    [nonce] (u64, little-endian)

In mode Plaintext the public key is all zero. The client's nonce is 64 random bits in both modes,
because ``REPLY_2`` echoes it; a Plaintext server's nonce is zero. In mode Required the public key
is a fresh ephemeral key per handshake attempt and the nonce is drawn at the same time. A Plaintext
crypt info never enters a key schedule: key derivation refuses a remote whose mode is not Required.
The client uses one key pair and one crypt info for every ``REQUEST_2`` of one connection request
(each Happy Eyeballs candidate address has its own), including retransmissions.

Mode agreement
~~~~~~~~~~~~~~

The two modes must be equal. A peer never falls back. The server compares the client's mode with its
own after the challenge check and, on a mismatch, allocates nothing and answers::

    [message ID] (u8)       // ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION or ID_OUR_SYSTEM_REQUIRES_SECURITY
    [OFFLINE_ID] (16 bytes)
    [server GUID] (u64)

25 bytes. The ID is chosen from the client's point of view: a ``Required`` server answers a
``Plaintext`` client with ``ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION``, and a ``Plaintext`` server
answers a ``Required`` client with ``ID_OUR_SYSTEM_REQUIRES_SECURITY``. The client also checks the
mode in ``REPLY_2`` and refuses, with the corresponding ID, if the server answered anyway. The
server's application is not notified.

Retransmission
~~~~~~~~~~~~~~

If ``REQUEST_2`` arrives again from an address and GUID that already has an unverified connection
from this handshake, the server compares the client crypt info byte for byte (constant time) with
the one it stored. If they are equal it resends ``REPLY_2`` with the **stored** server crypt info,
so a lost ``REPLY_2`` does not change the keys. If they differ, the datagram is dropped.

The challenge
-------------

The challenge is the stateless cookie of GameNetworkingSockets' UDP handshake. It proves that the
sender can receive packets at the address it claims, before the server spends anything on it.

Each server draws a random 16-byte secret ``K`` when its peer object is created and never sends it.
Let ``now`` be the server's monotonic clock in microseconds.

::

    bucket   = (u16) (now >> 20)            // 2^20 us, about 1.05 s per bucket
    input    = bucket (u16, native order)   // 20 bytes, packed
               | port (u16, network order)
               | ipv6 address (16 bytes)    // an IPv4 address as ::ffff:a.b.c.d
    h        = SipHash-2-4(K, input)        // crypto_shorthash, read as a little-endian u64
    challenge = (h & 0xFFFFFFFFFFFF0000) | bucket

The low 16 bits of the challenge are the bucket the server issued it in; the high 48 bits are the
keyed hash. Because ``K`` is private to one server, the layout of ``input`` is not a wire
format: it is documented here to describe the construction, and another implementation may choose
its own.

To verify a challenge ``c`` from a sender at ``now``::

    then    = (u16) c
    elapsed = (u16) (bucket(now) - then)
    reject if elapsed > 3                   // 3 = (4,000,000 us) >> 20
    accept if ((SipHash-2-4(K, input(then, sender)) & 0xFFFFFFFFFFFF0000) | then) == c

with a constant-time comparison. A challenge is therefore good for the bucket it was issued in and
the three that follow. Because the bucket boundary falls anywhere within the second in which it was
issued, that is a window of at least about 3.1 seconds and at most about 4.2 seconds. Unsigned 16-bit
subtraction makes the check correct across the bucket counter wrapping.

A challenge bound to the sender's address and port cannot be replayed from another address. It can
be replayed from the same address within the window, which is harmless: it only lets the sender
repeat ``REQUEST_2``, and the server answers a repeat with the same stored reply.

Key schedule
------------

The initiator and the responder each hold their own ephemeral X25519 secret key and the other's
public key. Let ``I`` be the initiator's crypt info and ``R`` the responder's, as serialized above
(41 bytes each, including the nonce).

1. **Shared secret.** ``shared = X25519(own secret, remote public)``, 32 bytes. If the result is all
   zero (a low-order remote key) the handshake fails: the server drops the ``REQUEST_2``, the
   client fails the attempt with ``ID_CONNECTION_ATTEMPT_FAILED``. The remote must also have sent
   mode Required.
2. **Extract.** ``salt = I.nonce (8 bytes LE) | R.nonce (8 bytes LE)``, 16 bytes.
   ``PRK = HMAC-SHA256(key = salt, message = shared)``. This is HKDF-Extract (RFC 5869).
3. **Expand.** The ``info`` string is 117 bytes::

       "MafiaNet datagram"         17 bytes of ASCII, no terminator
       protocol version             1 byte (9)
       initiator GUID               8 bytes, little-endian
       responder GUID               8 bytes, little-endian
       initiator crypt info I      41 bytes
       responder crypt info R      41 bytes
       direction                    1 byte

   Two keys are derived with HKDF-Expand (RFC 5869) at 32 bytes each, which is a single block
   ``T(1) = HMAC-SHA256(PRK, info | 0x01)``, where the ``0x01`` here is the HKDF block counter and
   is distinct from the direction byte inside ``info``:

   * direction ``0x01``: ``keyIR``, used to seal datagrams from the initiator to the responder.
   * direction ``0x02``: ``keyRI``, used to seal datagrams from the responder to the initiator.

   The initiator seals with ``keyIR`` and opens with ``keyRI``. The responder does the reverse.
4. **Wipe.** The ephemeral secret key, ``shared`` and ``PRK`` are wiped as soon as the keys are
   derived. The ephemeral secret never exists after the handshake.

The ``info`` binds the keys to everything the handshake exchanged: the protocol version, both GUIDs
and both crypt infos including both nonces. A different transcript gives different keys, and the
two directions have different keys, so a datagram cannot be reflected back at its sender.

The responder derives its keys when it processes ``REQUEST_2``; the initiator when it processes
``REPLY_2``. From that moment the responder expects every datagram from that address to be sealed.

Connected datagrams
-------------------

After the handshake every datagram of a ``Required`` connection has this layout::

    [prefix] (u8)
    [sequence] (n bytes, little-endian, n in [1,8])
    [ciphertext] (len bytes)
    [tag] (16 bytes)

``ciphertext`` is the encryption of one reliability-layer datagram, exactly as it would have been
sent in plaintext mode. The overhead is ``1 + n + 16`` bytes, at most 25. MafiaNet reserves 25 bytes
of every datagram for it, from the moment the connection is created, so that the size of the
plaintext never changes with the sequence number.

Prefix byte
~~~~~~~~~~~

::

    bit 7     (0x80)  always 1
    bits 6..4         n - 1, where n is the number of sequence bytes
    bits 3..0         zero

so ``prefix = 0x80 | ((n - 1) << 4)``, in the range ``0x80`` to ``0xF0``. The sender uses the
smallest ``n`` that holds the sequence number. A receiver accepts any ``n`` in ``[1,8]``.

Sequence number and nonce
~~~~~~~~~~~~~~~~~~~~~~~~~

Each direction has its own 64-bit sequence number. It starts at 0 for a new session, increases by
one per sealed datagram, and is consumed only once the datagram has been produced. ``2^64 - 1`` is
reserved and never sent: when the sequence reaches it the sender refuses to seal another datagram.
There is no rekeying.

The AEAD nonce is 12 bytes: four zero bytes followed by the sequence number as a little-endian
u64. Because the keys are fresh for every session and the sequence never repeats under a key, a
``(key, nonce)`` pair is never reused.

Associated data
~~~~~~~~~~~~~~~

Two bytes are authenticated but not encrypted::

    [protocol version] (u8)   // 9
    [prefix] (u8)

Authenticating the prefix binds the length of the sequence field; authenticating the protocol
version is defense in depth on top of the version already being in the key schedule.

Receive order
~~~~~~~~~~~~~

Once a connection has keys, a datagram from its address is handled in this order, and the first
failing step discards it::

    1. length <= 1400                       // MAXIMUM_MTU_SIZE; the datagram is copied here
    2. length >= 1
    3. prefix bit 7 is set and bits 3..0 are zero
    4. n = ((prefix >> 4) & 7) + 1;  length >= 1 + n + 16
    5. sequence = the n bytes, little-endian;  sequence != 2^64 - 1
    6. replay window says "not already received"
    7. AEAD open with the receive key, the nonce and the AAD; the tag verifies
    8. record the sequence in the replay window
    9. hand the plaintext to the reliability layer

Nothing is parsed, no plugin is notified, and the connection's "last heard from" time is not
updated before step 7 succeeds. Step 8 happens only after step 7, so a forged datagram can never
move the window. A datagram that fails any step is dropped, ``droppedUnauthenticatedDatagrams`` is
incremented, and the connection is not otherwise affected: no disconnect, no reply, no assertion.
The open is performed on a private copy, so a failed open leaves nothing behind.

Replay window
~~~~~~~~~~~~~

This is netcode's replay protection. The receiver keeps ``mostRecent`` (starting at 0) and an
array ``received[256]`` indexed by ``sequence % 256``, every entry initialised to ``2^64 - 1``
meaning empty.

::

    already_received(sequence):
        if sequence == 2^64 - 1:                          return true
        if mostRecent >= 256 and sequence <= mostRecent - 256:   return true   // too old
        slot = received[sequence % 256]
        if slot == 2^64 - 1:                              return false         // empty
        return slot >= sequence

    advance(sequence):                                    // only after the tag verified
        if sequence == 2^64 - 1:  return
        if sequence > mostRecent:  mostRecent = sequence
        received[sequence % 256] = sequence

A sequence is accepted at most once, and datagrams may arrive out of order as long as they are within
256 of the highest sequence seen. The comparison is written so that it cannot overflow near
``2^64 - 1``.

Plaintext mode
--------------

With mode Plaintext on both sides, the crypt infos carry zero keys (and only the client's nonce, for
the ``REPLY_2`` echo), no keys are derived,
no overhead is reserved, and connected datagrams are bare reliability-layer datagrams as they were
before protocol version 9. ``ID_CONNECTION_REQUEST`` is then the first datagram of the connection,
unsealed. ``droppedUnauthenticatedDatagrams`` stays 0.

Simultaneous connect
--------------------

If two peers call ``Connect`` to each other at once, two handshakes run in parallel and each peer
would otherwise answer the other's ``REQUEST_2`` and install keys from the other's handshake. The two
sides would end with keys from different handshakes and nothing could be opened. The rule that
prevents this, in ``Required`` mode:

* **The handshake initiated by the peer with the lower GUID wins on both sides** (compared as u64
  values).
* A peer whose own ``Connect`` to that address is pending, and whose GUID is lower than the sender's,
  **does not answer** the sender's ``REQUEST_2``. It drops it without allocating or deriving
  anything. The sender retries; once the lower peer's handshake has completed the retry is answered
  with ``ID_ALREADY_CONNECTED``, and if the lower peer's attempt failed it is answered normally.
* A peer with the higher GUID answers as usual. The keys it installs are the winner's. A later
  ``REPLY_2`` to its own attempt, whether stale, duplicated or forged, is ignored and never replaces
  installed keys, and the attempt is retired once the winner's ``ID_CONNECTION_REQUEST`` has opened.
* When the higher peer refuses the lower peer with ``ID_NO_FREE_INCOMING_CONNECTIONS``, it
  immediately retries its own attempt instead of waiting a full retry interval.

The application sees: the lower peer receives ``ID_CONNECTION_REQUEST_ACCEPTED``; the higher peer's
``Connect`` is absorbed into the incoming connection and resolves as ``ID_NEW_INCOMING_CONNECTION``,
or as ``ID_CONNECTION_ATTEMPT_FAILED`` if that connection ends before it completes. Keys are
installed once per remote system and are never replaced.

In ``Plaintext`` mode there are no keys to disagree about, and the handshakes converge as they
always did.

Security notes
--------------

* Nothing in the handshake is authenticated. An attacker on the path can substitute their own crypt
  info on each leg and relay the connection (see :doc:`../basics/secure-connections`).
* An off-path attacker that knows a connecting client's address and port can send a refusal ID
  (``ID_CONNECTION_BANNED``, a mode mismatch ID and the like) during the connect window and make that
  attempt fail. A forged ``REPLY_2`` needs the 64-bit nonce echo as well, which an off-path attacker
  does not see. Neither lets it learn or choose the session keys, because it does not hold the
  client's ephemeral secret.
* ``REQUEST_1`` is ignored unless it is at least as long as ``REPLY_1``, so the server cannot be used
  to amplify traffic towards a spoofed address.
* ``REPLY_1`` and ``REQUEST_2`` are unauthenticated and cheap. The challenge keeps the responder
  from allocating memory or doing a key exchange for a sender that cannot receive at its claimed
  address; it does not stop a flood from an address the attacker really controls. The responder
  runs its X25519 only after every cheap refusal, including ``SetLimitIPConnectionFrequency``'s
  "connected from this IP in the last 100 ms".
* A responder keys a connection when it processes ``REQUEST_2``, so from then on every datagram from
  that address must be sealed; a plaintext datagram is dropped and counted like any forgery.

Constants
---------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Constant
     - Value
   * - Protocol version
     - 9
   * - Crypt info size
     - 41 bytes (1 mode + 32 public key + 8 nonce)
   * - Key size, shared secret size
     - 32 bytes
   * - Tag size
     - 16 bytes
   * - AEAD nonce size
     - 12 bytes (4 zero bytes + 8-byte sequence)
   * - HKDF info label
     - ``MafiaNet datagram`` (17 bytes)
   * - HKDF info size
     - 117 bytes
   * - Direction bytes
     - ``0x01`` initiator to responder, ``0x02`` responder to initiator
   * - Prefix byte
     - ``0x80 | ((n - 1) << 4)``
   * - Maximum per-datagram overhead
     - 25 bytes
   * - Replay window
     - 256 sequences
   * - Reserved sequence
     - ``2^64 - 1``, never sent
   * - Challenge bucket
     - ``2^20`` microseconds
   * - Challenge acceptance
     - 0 to 3 buckets after issue (about 3.1 to 4.2 s)
   * - Maximum datagram
     - 1400 bytes (``MAXIMUM_MTU_SIZE``)
   * - Minimum ``REQUEST_1``
     - 35 bytes (the size of ``REPLY_1``)
   * - ``REPLY_2`` nonce echo
     - 8 bytes, right after ``OFFLINE_ID``
