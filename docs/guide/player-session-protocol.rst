Implement a complete player session
===================================

This walkthrough runs a MafiaNet server and client on loopback. The server uses
``MafiaNet::Server`` and the client uses its counterpart, ``MafiaNet::Client``.
The client presents a ticket, waits for admission, reports that its world is
loaded, learns its player number and leaves. You can also keep one client connected and watch a
second one wait while a staff member uses a reserved slot.

Build and run
-------------

From the MafiaNet repository root, with the dependencies in
:doc:`../getting-started/installation` installed, build both sample programs:

.. code-block:: shell

   cmake -S . -B build -DMAFIANET_BUILD_SAMPLES=ON -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release --target PlayerLogin PlayerLoginClient

Start the server in one terminal and leave it running:

.. code-block:: shell

   ./build/Samples/PlayerLogin/PlayerLogin

In a second terminal, run the client:

.. code-block:: shell

   ./build/Samples/PlayerLogin/PlayerLoginClient

With a Visual Studio multi-configuration build, run
``build\Samples\PlayerLogin\Release\PlayerLogin.exe`` and
``build\Samples\PlayerLogin\Release\PlayerLoginClient.exe``.
Both programs use IPv4 loopback and UDP port 60000. On a fresh server, the
client prints ``Connected``, then ``Joined as player 1`` and exits. The server
prints ``Player 1 joined`` and ``Player 1 left (Finished)``. Stop the server with Ctrl+C.

What crosses the connection
---------------------------

MafiaNet handles transport negotiation, encryption and reliable delivery.
The game defines the ticket bytes and the two typed messages below. The
session request and its answer travel before either end reports a connected
client. Each column shows the events that application's ``poll()`` delivers:

.. list-table::
   :header-rows: 1
   :widths: 12 41 47

   * - Stage
     - Client
     - Server
   * - Request
     - ``Client::connect(host, port, ticket)`` sends the ticket as the session payload and starts the attempt.
     - ``ConnectionRequested`` delivers the request payload, remote address and a generation-bound handle. Only pending capacity is occupied.
   * - Wait or deny
     - ``ConnectionStatus`` carries each update. A refusal arrives as ``ConnectionFailed`` with ``ConnectionFailure::Rejected``, the server's code and its detail.
     - ``request.status()`` keeps the request open. ``request.reject(code, detail)`` ends it. A pending peer leaving or timing out produces ``ConnectionAbandoned``.
   * - Accept
     - ``ConnectionAccepted`` carries the server's response. Game messages can be sent from then on.
     - ``request.accept(slot)`` claims a connected slot. ``Full`` leaves the same request queued. ``ClientConnected`` means the accepted client can exchange game messages.
   * - Ready
     - Send ``WorldLoaded{revision}`` after loading the world. The sample has no assets and sends it immediately.
     - Resolve the sender with ``server.connection(sender)``, check the revision and call ``accept_player()`` on that connection.
   * - Joined
     - Receive ``Welcome{playerNumber}``. This is when the client learns its assigned number.
     - ``PlayerJoined`` supplies the numbered player. Send the welcome through that handle.
   * - Leave
     - ``client.disconnect(code, detail)`` closes gracefully. ``ServerDisconnected`` reports how the session ended: ``Closed`` after this call, ``Disconnected`` with the server's code and detail after a kick, or ``ConnectionLost``.
     - A joined client's departure produces ``PlayerLeft`` followed by ``ClientDisconnected``, both with the client's code and detail. A connected client leaving before its join produces only ``ClientDisconnected``.

The session request, optional status updates, response and rejection use
MafiaNet's built-in session message IDs. On a raw server the request arrives as
``ID_SESSION_CONFIG_REQUEST`` and a successful handshake produces
``ID_NEW_INCOMING_CONNECTION``. On a raw client, ``ConnectionAccepted`` is
``ID_CONNECTION_REQUEST_ACCEPTED`` followed by ``GetRemoteSessionConfig()``, and
``ConnectionStatus`` is ``ID_SESSION_CONFIG_STATUS``. Treat session and rejection
payloads as length-delimited bytes. The sample chooses text for its tickets and
queue messages. See :doc:`../basics/connecting` for the underlying handshake.

Reason codes
------------

A refusal and a graceful close carry an application-defined ``uint32_t`` code
next to the text detail. Zero means no code. The game decides what the codes
mean; the sample defines them in its shared protocol header. The server passes
them to ``request.reject(code, detail)`` and ``disconnect(code, detail)`` on a
``ClientConnection`` or ``Player``. The client passes them to
``client.disconnect(code, detail)``. The other end reads them from
``ConnectionFailed``, ``ServerDisconnected``, ``PlayerLeft`` or
``ClientDisconnected``. The end that called ``disconnect()`` reports
``LeaveReason::Closed`` with the values it sent.

``ConnectionFailed`` also covers refusals that happen before the server
application sees the request. The table maps each ``ConnectionFailure`` to the
raw message it replaces:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - ``ConnectionFailure``
     - Raw message and cause
   * - ``Rejected``
     - ``ID_CONNECTION_ATTEMPT_FAILED`` with a body: the server application refused the session.
   * - ``NoResponse``
     - ``ID_CONNECTION_ATTEMPT_FAILED`` without a body: no answer, a session timeout, or a raw ``RejectSession()`` without a reason.
   * - ``ServerFull``
     - ``ID_NO_FREE_INCOMING_CONNECTIONS``: the server's waiting pool is full or closed.
   * - ``Banned``
     - ``ID_CONNECTION_BANNED``
   * - ``InvalidPassword``
     - ``ID_INVALID_PASSWORD``
   * - ``IncompatibleProtocol``
     - ``ID_INCOMPATIBLE_PROTOCOL_VERSION``
   * - ``ServerIdentityMismatch``
     - ``ID_PUBLIC_KEY_MISMATCH``: the server failed the identity pinned with ``pin_server_identity()``.
   * - ``EncryptionMismatch``
     - ``ID_REMOTE_SYSTEM_REQUIRES_ENCRYPTION`` or ``ID_OUR_SYSTEM_REQUIRES_SECURITY``
   * - ``AlreadyConnected``
     - ``ID_ALREADY_CONNECTED``
   * - ``RecentlyConnected``
     - ``ID_IP_RECENTLY_CONNECTED``

On the wire, the code and detail follow the message ID of the rejection or the
disconnect notification. With a zero code the body is the detail alone, exactly
what ``RejectSession()`` and ``CloseConnection()`` send, so raw peers and
``Server`` or ``Client`` read each other's text unchanged. A nonzero code is
framed as a zero byte, the code in four bytes in network byte order, then the
detail. A detail that begins with a zero byte, and a rejection with neither code
nor detail, use the same frame. Raw text never begins with a zero byte, so a
reader can tell the forms apart. A raw peer that wants to send or read a code
can produce and parse this frame itself.

Share the game messages
-----------------------

Compile this header into both programs. It assigns stable application IDs and
defines the field order for encoding and decoding. Both ends register
``WorldLoaded`` at the first ID and ``Welcome`` at the next. Register outbound
types with ``register_message`` and inbound types with ``on``. A local
``PlayerJoined`` callback reaches only the server; the welcome is the game's
explicit acknowledgement to the client.

.. literalinclude:: ../../Samples/PlayerLogin/protocol.h
   :language: cpp
   :caption: Samples/PlayerLogin/protocol.h

Make the admission decision on the server
-----------------------------------------

The example allows two connected clients, reserves one of those places for staff
and holds up to eight pending handshakes, with four per IP. The tickets
``guest-demo`` and ``staff-demo`` are fixed fixtures for this loopback example.
In a deployed game, verify the ticket with your account service and take the
slot class from the verified account. For an asynchronous check, retain the
original ``ConnectionRequest`` and return the result to the game thread.
A reconnect with the same GUID gets a new generation.

Every second, the sample retries waiting requests and sends a status to anyone
still waiting. It continues past a full public request so that a later eligible
staff member can take a free slot. Status updates restart the pending handshake
timeout on both ends. For a real whitelist service, cache the verified result by
connection key and clear outstanding work on ``ConnectionAbandoned``.
:doc:`admission-queue` describes sizing and queue ordering.

.. literalinclude:: ../../Samples/PlayerLogin/main.cpp
   :language: cpp
   :caption: Samples/PlayerLogin/main.cpp

Drive the client
----------------

``Client::connect()`` starts the peer and sends the ticket. For an identity pin,
a password or a custom bind address, build the peer with ``Peer::client()``,
pass the ticket to ``session()``, and move the started peer into ``Client``.
Register handlers, then call ``poll()`` while waiting, loading and playing.
Every attempt ends with one ``ConnectionAccepted`` or one ``ConnectionFailed``,
and an accepted session ends with one ``ServerDisconnected``. Game messages go
through ``client.messages()`` and ``client.send()``. An exhausted waiting pool
fails the attempt with ``ServerFull`` before the server can check the ticket.
After connection, wait for the typed welcome before exposing the player number.
A ``Client`` serves one session; create a new one to connect again.

.. literalinclude:: ../../Samples/PlayerLogin/client.cpp
   :language: cpp
   :caption: Samples/PlayerLogin/client.cpp

Exercise the other outcomes
---------------------------

Keep the same server running. In separate terminals, start the following clients
in order. Launch the third while the first is still holding its slot.

#. Run ``PlayerLoginClient guest-demo --hold``. It joins and stays for 20 seconds.
#. Run ``PlayerLoginClient guest-demo``. It prints ``Queue: Waiting for a slot``.
#. Run ``PlayerLoginClient staff-demo``. It joins through the reserved slot and leaves.
#. When the first guest leaves, the queued guest joins on its original connection.
#. Run ``PlayerLoginClient unknown``. It prints ``Connection failed: Unknown ticket``
   and exits with status 1. The server emits no player join for that attempt.
   The refusal also carries ``Login::unknownTicket``, which a game could use to
   pick a localized message.

Use ``Player`` handles for delayed sends or kicks. Retaining the original handle
binds the operation to that join, even if the peer reconnects before the network
thread executes it. For a replicated world, register matching replica types on
both endpoints. To begin streaming at game join, configure manual replication
admission before polling and call ``Admit(player.key().guid)`` from
``PlayerJoined``. See :doc:`replication` for the replication setup.
