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
       auto server = Server::listen(60000, 32);
       if (!server)
           return 1;
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

Admission and game join
-----------------------

The default policy accepts an incoming game client immediately. For a game
that waits for authentication or loading, pass ``JoinPolicy::Manual`` to
``Server::listen``. To inspect requests during the session handshake, also pass
``AdmissionPolicy::Interactive``. These policies control different decisions.

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Callback
     - Meaning
   * - ``ConnectionRequested``
     - Interactive admission has a request. Inspect its owned binary ``payload()``.
       Retain the request to defer the decision. Use ``status()`` to report progress
       and ``accept()`` or ``reject()`` to queue one final answer.
   * - ``ConnectionAbandoned``
     - A pending request went away. Match its ``ConnectionKey`` to cancel outstanding work.
   * - ``ClientConnected``
     - The incoming session is established. The handler receives a ``ClientConnection``.
       In manual mode call ``accept_player()`` when the game is ready.
   * - ``PlayerJoined``
     - The join acquired its server-local number. Repeated acceptance returns the
       existing number. The next poll dispatches the callback once, or the current
       poll dispatches it after the running handler returns.
   * - ``PlayerLeft``
     - A joined player departed. The snapshot retains its original number, connection
       key, disconnect reason and any reason payload, even across a reconnect.
   * - ``ClientDisconnected``
     - An established client departed. This arrives after ``PlayerLeft`` for a joined
       player. A client leaving mid-login produces only the client event.

A saved ``Player`` supports ``id()``, ``ping()``, ``send()`` and ``disconnect()``.
``server.player(id)`` looks up an active player. ``server.players()`` returns a
snapshot of accepted players in number order. Use the original handle for
deferred operations. The server also checks the connection generation when
applying a queued send, kick or admission decision. A reconnect with the same
GUID gets its own generation. Keys are local to the server instance. Destroying
the server expires its handles and clears its roster.

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
``Server`` before receiving its first packet. Use
``ServerBuilder::interactive_admission()`` to configure the handshake before startup.

Build the ``PlayerWelcome`` and ``PlayerLogin`` executables with
``MAFIANET_BUILD_SAMPLES=ON``. The first sends a typed welcome containing the
assigned number. The second checks a client-supplied world version during
admission, then waits for a typed load-complete message before admitting the
player to the game.
