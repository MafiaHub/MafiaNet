Player servers
==============

Use ``MafiaNet::Server`` for a listening game server. It owns the peer, dispatches
client and player events, and shares one receive loop with typed game messages.
``Player`` represents one accepted join. A player can change avatars or own
several replicas during that join.

.. code-block:: cpp

   #include "mafianet/server.h"
   #include <iostream>

   int main()
   {
       using namespace MafiaNet;
       auto server = Server::listen(60000, {32});
       if (!server)
           return 1;
       server->on<ConnectionRequested>([](const ConnectionRequest &request) {
           if (request.accept() == SessionAcceptResult::Full)
               request.reject("Server full");
       });
       server->on<ClientConnected>([](ClientConnection client) { client.accept_player(); });
       server->on<PlayerJoined>([](Player player) {
           std::cout << "Joined as player " << static_cast<uint32_t>(player.id()) << '\n';
       });
       server->on<PlayerLeft>([](const PlayerLeft &event) {
           std::cout << "Player " << static_cast<uint32_t>(event.player_id) << " left\n";
       });
       for (;;)
       {
           server->transport().WaitForPackets(16);
           server->poll();
       }
   }

Integrate ``poll()`` into your game tick. Callbacks run on the thread that
polls. Register handlers before polling. Send, disconnect, accept and query
handles on the same thread. For background login or asset loading, capture the
connection, do the work on the worker, then post the result and the original
handle back to the game thread. Only ``poll()`` should drain the server's packets.

Bounding a poll
---------------

``poll()`` runs until the transport has nothing left, so under a flood one tick can last as long as packets keep
arriving. ``poll(PollBudget{maxPackets, maxPacketsPerConnection})`` handles at most that many application packets
(zero means unbounded) and returns. The rest wait, in arrival order per connection, for the next call; the per-connection
share keeps one flooding connection from using the whole budget. ``Server::last_poll().backlog`` and ``Server::backlog()``
tell the game how much is waiting. Connection events do not count against the budget and are never reordered with their
own connection's packets.

Admission and game join
-----------------------

Every incoming session starts with ``ConnectionRequested``. The application must
accept or reject it. A request with no handler or no answer stays pending until
the session timeout. Acceptance produces ``ClientConnected``. Call
``accept_player()`` there, or retain the connection until login or loading
finishes. Only that call assigns a player number and produces ``PlayerJoined``.

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Callback
     - Meaning
   * - ``ConnectionRequested``
     - Inspect the remote endpoint with ``address()`` and the binary login data with ``payload()``.
       Retain the request to defer the decision. Use ``status()`` to report progress
       and ``accept()`` or ``reject()`` to queue one final answer.
   * - ``ConnectionAbandoned``
     - A pending request went away. Match its ``ConnectionKey`` to cancel outstanding work.
   * - ``ClientConnected``
     - The incoming session is established. The handler receives a ``ClientConnection``.
       Call ``accept_player()`` when the game is ready.
   * - ``PlayerJoined``
     - The join acquired its server-local number. Repeated acceptance returns the
       existing number. The next poll dispatches the callback once, or the current
       poll dispatches it after the running handler returns.
   * - ``PlayerLeft``
     - A joined player departed. The snapshot retains its original number, connection
       key, leave reason, reason code and detail, even across a reconnect.
   * - ``ClientDisconnected``
     - An established client departed. This arrives after ``PlayerLeft`` for a joined
       player. A client leaving mid-login produces only the client event.

A saved ``Player`` supports ``id()``, ``ping()``, ``send()`` and ``disconnect()``.
``disconnect(code, detail)`` and ``request.reject(code, detail)`` send an
application-defined reason code with the text. The client reads both, and the
server reports its own close as ``LeaveReason::Closed`` with the same values.
See :doc:`player-session-protocol` for the codes and their wire format.
``server.player(id)`` looks up an active player. ``server.players()`` returns a
snapshot of accepted players in number order. Use the original handle for
deferred operations. The server also checks the connection generation when
applying a queued send, kick or admission decision. A reconnect with the same
GUID gets its own generation. Keys are local to the server instance. Destroying
the server expires its handles and clears its roster.

Use ``server.pending_requests()`` to inspect unanswered requests in callback
order. A full server can still check a whitelist, send queue updates, and admit
reserved callers. See :doc:`admission-queue` for capacity sizing and retry handling.

Limiting what clients send
--------------------------

Pass an ``InboundRateLimit`` to ``Server::listen()`` (or ``ServerBuilder::inbound_rate_limit()``,
or ``SetInboundRateLimit()`` on any peer) to bound the messages each client may send. The network
thread enforces it before a message is queued, so a flooding client costs the game thread nothing.

.. code-block:: cpp

   InboundRateLimit limit;
   limit.messages_per_second = 50;   // sustained rate
   limit.burst = 100;                // accepted at once above it
   limit.flood_per_second = 500;     // more than this in one second is a flood
   limit.flood_action = FloodAction::Disconnect;
   limit.flood_reason = "flood";     // sent to the client, opaque to MafiaNet
   auto server = Server::listen(60000, {32}, limit);

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Field
     - Meaning
   * - ``messages_per_second``, ``burst``
     - A token bucket per connection. It starts full with ``burst`` messages (``messages_per_second``
       when ``burst`` is zero) and refills at ``messages_per_second``. Messages beyond it are dropped.
   * - ``flood_per_second``
     - Counts every message in a one-second window, dropped ones included. The message that crosses
       it starts a flood: the rest of that window is dropped, and ``FloodAction::Disconnect`` also closes
       the connection.
   * - ``flood_reason``
     - With ``FloodAction::Disconnect``, the client receives it in ``ID_DISCONNECTION_NOTIFICATION``.
       The server reports ``PlayerLeft`` and ``ClientDisconnected`` with ``LeaveReason::InboundFlood``
       and the reason as their detail, so the game can log the client or ban its address.
       The client receives it as the detail of ``ServerDisconnected``. A ``Client`` whose own limit closes
       a flooding server reports ``ServerDisconnected`` with ``LeaveReason::InboundFlood``.

Only application messages count: identifiers from ``ID_USER_PACKET_ENUM`` up, also behind an
``ID_TIMESTAMP``. Connection upkeep, the session handshake, RPC, replication and plugin traffic are
never limited. Each limit is off at zero. ``RakNetStatistics::rateLimitedMessages`` and
``inboundFloods`` count what each connection lost. A raw peer recognizes its own flood close by
``Packet::disconnectCause == DisconnectCause::InboundFlood`` on the local ``ID_DISCONNECTION_NOTIFICATION``.
``SetInboundRateLimit()`` may be called at any time and applies from the next network update.

Messages and replication
------------------------

Use ``server.messages()`` to register your normal wire messages with MafiaNet's
``Dispatcher``. Register send-only types with ``register_message<T>(id)``. Both
ends must agree on the wire ID. Local event handlers have their own table.
Inside a message handler, ``server.connection(sender)`` resolves the sending
session while it is still current. A typed send returns its queued receipt, or
zero if the handle expired, the type was unregistered, or queueing failed.

Use ``server.transport().GetReplication()`` to configure replication and its
admission policy independently. For an existing server with custom bind or
identity settings, move the ``Peer`` returned by ``Peer::server().start()`` into
``Server`` before receiving its first packet.

``MafiaNet::Client`` is the matching client: it connects with a session payload,
reports queue status and a typed connect outcome, and sends game messages through
the same ``Dispatcher``. For a runnable client and server, follow
:doc:`player-session-protocol`.
The ``PlayerWelcome`` sample also demonstrates assigning a player number as
soon as the connection is accepted.
