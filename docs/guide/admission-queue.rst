Admission and Queues
====================

A server often has to decide whether a client may join before it lets the client in: check a
whitelist or a ban list, validate a one-time ticket against a web service, or hold the client in a
queue until a slot frees up. MafiaNet lets the server make that decision **before either side reports
a connection**, with the waiting clients kept out of the player slots entirely.

This page explains how the pieces fit together and walks through a complete queue. The primitives
are the interactive session handshake (:doc:`../basics/connecting`) plus four additions built for
exactly this:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - API
     - Role
   * - ``SetMaximumPendingSessions(total, perAddress)``
     - Waiting clients live in a pool of their own instead of in the player slots.
   * - ``SetSessionTimeout(ms)``
     - How long a request may wait for its answer, on either end.
   * - ``SendSessionStatus(guid, data, length)``
     - Tell a waiting client what is happening; restarts the timeout on both ends.
   * - ``ID_SESSION_CONFIG_STATUS``
     - What the client receives from ``SendSessionStatus()``.
   * - ``ID_SESSION_CONFIG_ABANDONED``
     - Tells the server a request it was holding went away unanswered.

Why not decide after connecting?
--------------------------------

The obvious approach -- accept the connection, then check the client and kick it if it fails -- has
two problems.

* **The client already got in.** By the time it is kicked it has been told it is connected, has
  received whatever the server sends on connect, and may have started loading. A refused player
  sees the world flash up and then get torn down.
* **Waiting costs a slot.** A connected client holds one of the ``SetMaximumIncomingConnections()``
  slots and appears in ``NumberOfConnections()`` for as long as the check takes. A slow database
  lookup, a queue, or an attacker who connects and never finishes, all hold slots that real players
  need, and inflate the player count a server list shows.

Deciding inside the session handshake fixes the first: nothing is reported, and nothing but
handshake traffic crosses the connection, until the server answers. The pending pool fixes the
second.

How the pending pool works
--------------------------

Every remote peer that asked to connect but is not yet ``CONNECTED`` is **pending** -- from its first
open-connection request, through the transport handshake, to the session handshake waiting on your
answer. Without a pool, pending peers in the session handshake count against
``SetMaximumIncomingConnections()`` alongside connected ones, which is the historic behaviour and
stays the default.

With a pool:

* **Pending peers count against the pool**, ``total`` of them at once, at most ``perAddress`` from any
  one IP address. A newcomer that would exceed either is refused at the transport with
  ``ID_NO_FREE_INCOMING_CONNECTIONS``, exactly as a full server always refused one.
* **The incoming limit counts connected peers only.** ``NumberOfConnections()`` and
  ``GetSystemList()`` never include a pending peer, so neither does anything built on them -- a
  status command, a server browser's player count.
* **A handshake may start while the server is full.** That is what makes a queue possible: the
  client waits in the pool while every player slot is taken.
* **Acceptance is bounded by the incoming limit.** An ``AcceptSession()`` that would take the
  connected count past ``SetMaximumIncomingConnections()`` refuses the peer instead (the client sees
  ``ID_CONNECTION_ATTEMPT_FAILED`` with no reason). Check ``NumberOfConnections()`` yourself first and
  refuse with a reason of your own; the library check is the backstop for two accepts racing one
  free slot.

A flood of clients that connect and stall -- or just a long queue -- fills the pool and nothing else.
Players already connected keep playing, and free player slots stay free.

``Startup()`` allocates one connection structure per slot, so it must leave room for both:

.. code-block:: cpp

   const unsigned short maxPlayers = 64;
   const unsigned short maxWaiting = 32;

   server->SetSessionConfigInteractive(true);
   server->SetMaximumPendingSessions(maxWaiting, 4);   // at most 4 waiting from one IP
   server->SetSessionTimeout(45000);                   // how long an answer may take

   MafiaNet::SocketDescriptor sd(60000, 0);
   server->Startup(maxPlayers + maxWaiting, &sd, 1);   // room for players AND waiting clients
   server->SetMaximumIncomingConnections(maxPlayers);  // players only

.. note::

   The per-address share is per IP, not per player. Players behind one NAT -- a household, a LAN
   party, some mobile carriers -- share an address. Keep ``perAddress`` above 1 unless you mean to
   allow one waiting client per network.

The life of a waiting request
-----------------------------

.. code-block:: text

   client                                          server
     | -- open connection (transport) ------------->|  counted in the pending pool
     | -- ID_SESSION_CONFIG_REQUEST (payload) ----->|  surfaced to the application
     |<-- ID_SESSION_CONFIG_STATUS (0..n) -----------|  SendSessionStatus(); timeout restarts
     |                                               |
     |<-- ID_SESSION_CONFIG --------------------------|  AcceptSession(): now CONNECTED
     |    ID_CONNECTION_REQUEST_ACCEPTED              |  ID_NEW_INCOMING_CONNECTION
     |                                               |
     |    or                                          |
     |<-- ID_SESSION_CONFIG_REJECTED (reason) --------|  RejectSession(): never connected
     |    ID_CONNECTION_ATTEMPT_FAILED + reason       |
     |                                               |
     |    or the client leaves / the timeout passes   |
     |    ID_CONNECTION_ATTEMPT_FAILED                |  ID_SESSION_CONFIG_ABANDONED

While a request waits:

* ``GetConnectionState(guid)`` reports ``IS_CONNECTING`` on both ends.
* ``Send()`` to the waiting guid returns 0, and broadcasts skip it: the application has not been
  told this connection exists. Handshake messages, status included, go out regardless.
* Application data the waiting client pushes at the server is dropped, not delivered.

Every request ends exactly one way on the server: you answer it, or you receive
``ID_SESSION_CONFIG_ABANDONED`` for it -- once, and never for a request you already answered.
Answering after that does nothing.

Timeouts
--------

``SetSessionTimeout()`` bounds how long a request may go without an answer. It replaces the
connection timeout for the session handshake only; 0, the default, keeps using the connection
timeout. **Every** ``SendSessionStatus()`` restarts it, on the server when it is sent and on the
client when it arrives, so a queue that updates its clients regularly is never timed out however long
they wait.

Set it on both ends. Whichever end runs out first ends the attempt: a server prepared to wait 45
seconds does not help a client that gives up after the 10-second default. Clients should be generous
-- the server decides, and a dead server is caught by the ordinary connection timeout anyway:

.. code-block:: cpp

   client->SetSessionTimeout(120000);
   client->Connect("game.example.com", 60000, nullptr, 0);

When the server's timeout passes, the client gets ``ID_CONNECTION_ATTEMPT_FAILED`` with no reason and
the server gets ``ID_SESSION_CONFIG_ABANDONED``. If you want the client to read why, refuse it
yourself before the library's timeout does, and keep the library's value as the backstop.

Status messages
---------------

``SendSessionStatus()`` sends an opaque payload -- a UTF-8 line, a JSON object, a queue position as
bytes -- to a client whose request is still waiting. The client receives it as
``ID_SESSION_CONFIG_STATUS`` with the payload at ``packet->data + 1``:

.. code-block:: cpp

   // client
   case ID_SESSION_CONFIG_STATUS: {
       const std::string status((const char *)packet->data + 1, packet->length - 1);
       ShowConnectingScreen(status);   // "You are 3rd in the queue"
       break;
   }

Status is sent reliably and in order. It is ignored for a request that is no longer waiting, so a
status racing an accept is simply dropped. It is bounded by ``MAXIMUM_SESSION_CONFIG_SIZE``. A client
cannot send one to a server: the server ignores it.

A complete queue
----------------

The pattern: every request goes into a FIFO; each tick the server lets in as many as there are free
slots, tells everyone still waiting where they stand, and forgets anyone who left.

.. code-block:: cpp

   struct Waiting {
       MafiaNet::RakNetGUID guid;
       MafiaNet::TimeMS lastStatus = 0;
   };
   std::deque<Waiting> queue;
   unsigned int pendingAccepts = 0;   // accepted, not yet reported as ID_NEW_INCOMING_CONNECTION

   void OnPacket(MafiaNet::Packet *packet) {
       switch (packet->data[0]) {
       case ID_SESSION_CONFIG_REQUEST: {
           // The client's session payload rides the request: who it is, a ticket, a build id.
           if (!IsAllowed(packet->data + 1, packet->length - 1)) {
               server->RejectSession(packet->guid, "You are not on the whitelist.");
               break;
           }
           queue.push_back({packet->guid});
           break;
       }
       case ID_SESSION_CONFIG_ABANDONED:
           // Gave up or timed out. Drop it; everyone behind moves up.
           queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Waiting &w) { return w.guid == packet->guid; }), queue.end());
           break;
       case ID_NEW_INCOMING_CONNECTION:
           // An accepted request became a connection: it is counted by NumberOfConnections() now.
           --pendingAccepts;
           break;
       }
   }

   void Tick() {
       // Let in whoever fits.
       while (!queue.empty() && server->NumberOfConnections() + pendingAccepts < maxPlayers) {
           server->AcceptSession(queue.front().guid, sessionConfig.data(), (unsigned int)sessionConfig.size());
           ++pendingAccepts;
           queue.pop_front();
       }

       // Tell the rest where they stand, well inside the session timeout.
       const MafiaNet::TimeMS now = MafiaNet::GetTimeMS();
       for (size_t i = 0; i < queue.size(); ++i) {
           if (now - queue[i].lastStatus < 5000)
               continue;
           const std::string status = "You are " + std::to_string(i + 1) + " in the queue.";
           server->SendSessionStatus(queue[i].guid, status.data(), (unsigned int)status.size());
           queue[i].lastStatus = now;
       }
   }

Things to note in that loop:

* **Count accepts in flight.** ``AcceptSession()`` is queued to the network thread, so
  ``NumberOfConnections()`` does not include a peer you accepted a moment ago. Without
  ``pendingAccepts`` two ticks can accept into the same free slot; the library refuses the second,
  but that player is then turned away instead of kept waiting.
* **Status doubles as the keep-alive.** Each one restarts both timeouts, so the interval only has to
  be comfortably shorter than ``SetSessionTimeout()``. A client that stops getting status is timed
  out and shows up as ``ID_SESSION_CONFIG_ABANDONED``.
* **The pool bounds the queue.** Once ``total`` clients wait, the next one is refused with
  ``ID_NO_FREE_INCOMING_CONNECTIONS`` before it ever reaches ``ID_SESSION_CONFIG_REQUEST``. Size the
  pool for the longest queue you are willing to hold.
* **Never block the receive loop.** A decision that needs a database or an HTTP call is started
  when the request arrives and answered when the result comes back; the request waits in the pool
  meanwhile, with status to keep it alive if the call is slow.

Security notes
--------------

* The client's session payload, and anything in it, is untrusted input. Validate it before use; see
  :doc:`../advanced/session-handshake-security`.
* The pool is a bound, not a filter: anyone who can reach the port can occupy a pending slot until
  it is answered or times out. Keep the timeout proportionate, keep the per-address share small, and
  refuse early -- a request refused on arrival frees its slot at once.
* Status is visible to the waiting client only. Do not put anything in it you would not show a
  player who is about to be refused.

See Also
--------

* :doc:`../basics/connecting` - The session handshake and interactive mode
* :doc:`../advanced/session-handshake-security` - Trust model and denial-of-service limits
* :doc:`../basics/network-messages` - ``ID_SESSION_CONFIG_*`` message reference
* :doc:`client-server` - Client-server setup
