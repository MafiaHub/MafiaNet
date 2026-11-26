Whitelist checks, queues and reserved slots
===========================================

A listening game server has two budgets. A new arrival uses one pending slot
while the game checks its credentials. Acceptance claims one connected slot,
including while the network thread finishes the handshake. A connected client
gets a player number when the game calls ``accept_player()``.

.. code-block:: cpp

   ServerCapacity capacity;
   capacity.connected = 64;
   capacity.reserved = 4;
   capacity.pending = 32;
   capacity.pending_per_address = 4;
   auto server = Server::listen(60000, capacity);

This server can hold 64 connected clients and another 32 pending handshakes.
Up to 60 public clients can connect. A reserved caller can use any available
connected slot. For example, four connected staff still leave room for 60
public clients. All 64 could also be staff. The per-address limit counts
waiting handshakes sharing one IP, including clients behind the same NAT.
The default waiting budget is 32 with an unrestricted per-address share.

Deciding who gets in
--------------------

Register a ``ConnectionRequested`` handler before polling. Every incoming client
that reaches this stage follows the same pipeline. Inspect ``request.address()``
and the opaque bytes in ``request.payload()``. For an asynchronous whitelist
lookup, retain the request, post the verified result back to the game thread,
and apply it to that handle. Use the verified account's privileges to choose
the slot class. A claimed identity in the incoming payload is client input.

.. code-block:: cpp

   // Called on the game thread when the whitelist lookup completes.
   void on_whitelist_result(ConnectionRequest request, bool allowed, bool staff)
   {
       if (!allowed) {
           request.reject("This account is not on the whitelist");
           return;
       }
       auto slot = staff ? ConnectionSlot::Reserved : ConnectionSlot::Public;
       if (request.accept(slot) == SessionAcceptResult::Full)
           request.status("Verified. Waiting for a slot");
   }

An accepted request consumes capacity before the next call to ``poll()``.
Two callbacks competing for one free slot can therefore act on the result immediately.
``Full`` leaves the request open. ``Expired`` means the request was answered,
the client left, or the server closed. ``InvalidPayload`` and ``OutOfMemory``
leave the decision open. Both acceptance and rejection remove a decided request
from ``pending_requests()``. Losing a pending client also removes it and
produces ``ConnectionAbandoned`` with the original connection key. A waiting
client that gives up calls ``CancelConnectionAttempt()``. The server sees
``ConnectionAbandoned`` at once, or ``ClientDisconnected`` if it accepted the
request meanwhile, and the client never reports the connection.

Serving the queue
-----------------

``server.pending_requests()`` returns a snapshot in callback order. It includes
clients still undergoing a whitelist check. The game keeps verified privileges
under the original ``ConnectionKey`` and chooses which verified requests to retry.
Retry after whitelist results, ``ClientDisconnected``, or ``ConnectionAbandoned``
if a handshake ended after claiming capacity:

.. code-block:: cpp

   for (const ConnectionRequest &request : server.pending_requests()) {
       // Game-owned lookup returns optional<ConnectionSlot> for verified requests.
       if (auto slot = verified_slot(request.key()))
           request.accept(*slot);
   }

Continue past a full public request when a later staff member may use a reserved
slot. The game can report queue position through ``request.status(text)``.
The client receives ``ID_SESSION_CONFIG_STATUS``. Each update restarts the session
timeout at both ends. Choose an update interval shorter than both timeouts.
An earlier unanswered request times out if neither a decision nor a status arrives.

Using RakPeerInterface directly
-------------------------------

``Peer::server().capacity(capacity).start()`` sizes both populations. For manual
startup, configure ``SetMaximumPendingSessions(pending, perAddress)`` and
``SetMaximumIncomingConnections(connected, reserved)``, then pass the sum of
connected and pending capacities to ``Startup()``. The physical slot limit is
65,535. A full pending pool or transport returns ``ID_NO_FREE_INCOMING_CONNECTIONS``
before delivering an application request.

Answer ``ID_SESSION_CONFIG_REQUEST`` using ``AcceptSession()`` or
``RejectSession()``. The raw accept returns the same result and observes the same
reservations. Raw address and GUID calls select the request current at the time
of the call. Retain a ``ConnectionRequest`` when doing asynchronous work that
can outlive a reconnect. Use ``SendSessionStatus()`` to update a waiting client.
