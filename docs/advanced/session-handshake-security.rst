Session Handshake Security
==========================

The session handshake moves an application payload across the wire *before* either peer reports a
connection. This page states what that payload can and cannot be trusted to be, what MafiaNet
guarantees about it, and where the responsibility moves to the application.

Read this before parsing ``GetRemoteSessionConfig()`` output.

Trust model
-----------

**The payload is untrusted input.** By the time it is stored, the sending peer has completed the
offline connection handshake, matched ``RAKNET_PROTOCOL_VERSION``, and passed
``SetIncomingPassword()`` if one is set. That is the entire bar. In particular the payload is:

* **Unauthenticated** unless the build enables ``LIBCAT_SECURITY`` and the connection uses a public
  key. Without that, MafiaNet traffic is neither encrypted nor authenticated, so a peer that can
  spoof the source address of an in-progress connection can supply the payload instead.
* **Arbitrary bytes.** MafiaNet never inspects, parses, validates, or transforms it. Do not assume
  text, encoding, structure, or a trailing terminator.
* **Attacker-chosen in length**, up to ``MAXIMUM_SESSION_CONFIG_SIZE``.

MafiaNet treats the payload as an opaque blob: one bounded ``memcpy`` in, one pointer and length
out. There is no parser in the library, so the library itself presents no payload-driven
memory-safety surface. **All parsing risk lives in the application**, and a memory-unsafe parser
reached from ``GetRemoteSessionConfig()`` is remotely reachable pre-authentication.

What MafiaNet guarantees
------------------------

* **Length is bounded.** ``MAXIMUM_SESSION_CONFIG_SIZE`` (64 KB) is enforced on send *and* receive.
  An oversized inbound payload is treated as a protocol violation and the connection is dropped
  without a reply.
* **No length underflow.** The receive loop only dispatches frames of at least one byte, so the
  payload length (frame size minus the one-byte id) cannot wrap.
* **The buffer is NUL-terminated past its reported length.** One extra zero byte is always
  allocated and is never included in the length. This is defence in depth for applications that
  reach for a C-string API; ``length`` remains the authoritative bound.
* **Messages are bound to the role allowed to send them.** ``ID_SESSION_CONFIG`` and
  ``ID_SESSION_CONFIG_REJECTED`` are acted on only by the peer that *initiated* the connection, so a
  client cannot push a payload at a server or forge ``ID_CONNECTION_ATTEMPT_FAILED`` into a
  listening server's queue. In a simultaneous (cross-connection) handshake both peers are initiators
  and both accept them, which is correct.
* **The handshake cannot be replayed.** A second ``ID_SESSION_CONFIG_REQUEST`` on a connection that
  has left ``EXCHANGING_SESSION_DATA``, or one that arrives while a decision is already pending, is
  discarded. A peer cannot re-run the exchange to make the connection packet be produced twice.
* **A stalled handshake is bounded.** ``EXCHANGING_SESSION_DATA`` is timed out using the
  connection's own ``SetTimeoutTime()`` value.
* **Handshaking peers count against the incoming-connection limit.** They already own a slot, so
  ``SetMaximumIncomingConnections()`` is enforced against them too. (``GetNumberOfRemoteInitiatedConnections()``
  still reports only fully established peers, which is what an application means by "players".)

What the application must do
----------------------------

**Validate before you trust.** The payload is the *input* to your decision, never the decision. In
interactive mode the client's payload arrives before the server answers precisely so it can be
checked; a rejected peer never becomes a connection.

**Parse defensively.** Treat it exactly as you would a packet from an unauthenticated remote:

* Bound every read against ``length``. Do not rely on the terminator to stop a loop.
* Do not feed it to a parser that is not hardened against hostile input. A JSON, XML, or
  deserialization library reached here is reachable by anyone who can complete a connection
  handshake, before any application-level authentication has run.
* Never treat it as a path, a command, a format string, or code. A payload used to select a file to
  load must be resolved against a fixed root and rejected if it escapes.
* Do not size an allocation from a length field inside the payload without checking it against the
  actual ``length``.

**Do not put secrets in a server payload.** It is sent to every peer that completes the transport
handshake, before any application-level authentication. In static mode that is unconditional. If a
value should only reach authorized clients, use interactive mode and send it from
``AcceptSession()`` after validating that peer, or send it as a normal message after the connection
is established and authenticated.

**Prefer the GUID form when answering.** ``AcceptSession()``/``RejectSession()`` take an
``AddressOrGUID``. A system address can be reused by a different peer between the request arriving
and the answer being queued; a GUID cannot. Use ``packet->guid`` from the
``ID_SESSION_CONFIG_REQUEST`` packet.

**Answer every request.** Under ``SetSessionConfigInteractive(true)`` a peer that is never answered
holds a connection slot until the timeout expires, and the application never sees it. Make sure
every code path reaches ``AcceptSession()`` or ``RejectSession()``.

Denial of service
-----------------

**Memory.** Each connection can hold one payload, so the worst case a peer set can force is
``maxConnections`` × ``MAXIMUM_SESSION_CONFIG_SIZE`` (with the default cap, 64 KB per connection).
This is a new per-connection ceiling introduced by the handshake. If a deployment does not need
large payloads, lower ``MAXIMUM_SESSION_CONFIG_SIZE`` at build time.

**Slots.** A peer can complete the transport handshake and then simply not send its payload,
occupying a slot for the timeout duration. This is bounded by ``SetMaximumIncomingConnections()``
(handshaking peers are counted) and by ``SetTimeoutTime()``, but note the application is **not**
notified about these peers, so application-level defences keyed on ``ID_NEW_INCOMING_CONNECTION`` --
per-IP rate limits, ban checks, connection logging -- do not see them. A deployment that relies on
such defences should keep the timeout short and treat ``SetMaximumIncomingConnections()`` as the
real bound.

**Interactive mode extends the window.** With interactive mode on, how long a peer can hold a slot
depends on how quickly the application answers. Answer from the ``Receive()`` loop rather than
deferring behind slow work such as a database lookup or an HTTP call; if validation must be slow,
reject fast and let the peer retry rather than holding the handshake open.

Not addressed here
------------------

The session handshake does not change MafiaNet's existing transport security posture. Off-path
packet injection, address spoofing, amplification via the offline ping path, and the absence of
encryption when ``LIBCAT_SECURITY`` is disabled are unchanged and out of scope for this page. See
:doc:`../basics/secure-connections`.
