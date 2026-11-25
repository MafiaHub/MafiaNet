Implement a complete player session
===================================

This walkthrough runs a MafiaNet server and client on loopback. The client
presents a ticket, waits for admission, reports that its world is loaded, learns
its player number and leaves. You can also keep one client connected and watch a
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
prints ``Player 1 joined`` and ``Player 1 left``. Stop the server with Ctrl+C.

What crosses the connection
---------------------------

MafiaNet handles transport negotiation, encryption and reliable delivery.
The game defines the ticket bytes and the two typed messages below. The
session request and its answer travel before either end reports a connected
client. Each column shows what that application's receive loop observes:

.. list-table::
   :header-rows: 1
   :widths: 12 41 47

   * - Stage
     - Client
     - Server
   * - Request
     - Set the session payload before connecting. ``CONNECTION_ATTEMPT_STARTED`` means the attempt is underway.
     - ``ConnectionRequested`` delivers the request payload, remote address and a generation-bound handle. Only pending capacity is occupied.
   * - Wait or deny
     - ``ID_SESSION_CONFIG_STATUS`` carries an update. A rejection arrives as ``ID_CONNECTION_ATTEMPT_FAILED`` with the optional reason after the message ID.
     - ``request.status()`` keeps the request open. ``request.reject()`` ends it. A pending peer leaving or timing out produces ``ConnectionAbandoned``.
   * - Accept
     - Wait for ``ID_CONNECTION_REQUEST_ACCEPTED`` before sending game messages.
     - ``request.accept(slot)`` claims a connected slot. ``Full`` leaves the same request queued. ``ClientConnected`` means the accepted client can exchange game messages.
   * - Ready
     - Send ``WorldLoaded{revision}`` after loading the world. The sample has no assets and sends it immediately.
     - Resolve the sender with ``server.connection(sender)``, check the revision and call ``accept_player()`` on that connection.
   * - Joined
     - Receive ``Welcome{playerNumber}``. This is when the client learns its assigned number.
     - ``PlayerJoined`` supplies the numbered player. Send the welcome through that handle.
   * - Leave
     - The sample calls ``Shutdown(300)`` to allow the disconnect notification to leave. A remote close or a lost connection also ends the client loop.
     - A joined client's departure produces ``PlayerLeft`` followed by ``ClientDisconnected``. A connected client leaving before its join produces only ``ClientDisconnected``.

The session request, optional status updates, response and rejection use
MafiaNet's built-in session message IDs. On a raw server the request arrives as
``ID_SESSION_CONFIG_REQUEST`` and a successful handshake produces
``ID_NEW_INCOMING_CONNECTION``. If you include response bytes in
``request.accept(slot, response)``, the client can read them with
``GetRemoteSessionConfig()`` after acceptance. Treat session and rejection
payloads as length-delimited bytes. The sample chooses text for its tickets and
queue messages. See :doc:`../basics/connecting` for the underlying handshake.

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

Drive the client receive loop
-----------------------------

Set the ticket before ``Connect()``. Pump the peer while waiting, loading and
playing. The client handles transport events first and dispatches game messages
through its message table. An exhausted waiting pool produces
``ID_NO_FREE_INCOMING_CONNECTIONS`` before the server can check the ticket.
After connection, wait for the typed welcome before exposing the player number.
A fresh connection repeats the entire sequence.

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

Use ``Player`` handles for delayed sends or kicks. Retaining the original handle
binds the operation to that join, even if the peer reconnects before the network
thread executes it. For a replicated world, register matching replica types on
both endpoints. To begin streaming at game join, configure manual replication
admission before polling and call ``Admit(player.key().guid)`` from
``PlayerJoined``. See :doc:`replication` for the replication setup.
