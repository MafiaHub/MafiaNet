Connecting
==========

After starting MafiaNet, you can connect to remote systems.

Initiating a Connection
-----------------------

.. code-block:: cpp

   ConnectionAttemptResult Connect(
       const char *host,
       unsigned short remotePort,
       const char *passwordData,
       int passwordDataLength,
       PublicKey *publicKey = 0,
       unsigned connectionSocketIndex = 0,
       unsigned sendConnectionAttemptCount = 6,
       unsigned timeBetweenSendConnectionAttemptsMS = 1000,
       TimeMS timeoutTime = 0
   );

Basic usage:

.. code-block:: cpp

   MafiaNet::ConnectionAttemptResult result = peer->Connect(
       "192.168.1.100",  // IP address or hostname
       60000,            // Port
       nullptr,          // No password
       0                 // Password length
   );

   if (result != MafiaNet::CONNECTION_ATTEMPT_STARTED) {
       printf("Failed to initiate connection: %d\n", result);
   }

Connection Parameters
---------------------

host
~~~~

IP address (e.g., "192.168.1.100") or hostname (e.g., "game.example.com").

remotePort
~~~~~~~~~~

The port the remote system is listening on (set in their ``Startup()`` call).

passwordData / passwordDataLength
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Optional password for authentication:

.. code-block:: cpp

   const char* password = "secretpassword";
   peer->Connect("192.168.1.100", 60000, password, strlen(password));

The remote system must call ``SetIncomingPassword()`` with the same password.

sendConnectionAttemptCount / timeBetweenSendConnectionAttemptsMS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Control retry behavior. Default: 6 attempts, 1 second apart.

timeoutTime
~~~~~~~~~~~

How long to wait for connection before giving up (milliseconds). 0 = use default.

Handling Connection Results
---------------------------

Check for connection packets in your receive loop:

.. code-block:: cpp

   MafiaNet::Packet* packet;
   for (packet = peer->Receive(); packet;
        peer->DeallocatePacket(packet), packet = peer->Receive()) {

       switch (packet->data[0]) {
           case ID_CONNECTION_REQUEST_ACCEPTED:
               printf("Connected to %s\n",
                      packet->systemAddress.ToString());
               break;

           case ID_CONNECTION_ATTEMPT_FAILED:
               printf("Connection attempt failed\n");
               break;

           case ID_NO_FREE_INCOMING_CONNECTIONS:
               printf("Server is full\n");
               break;

           case ID_INVALID_PASSWORD:
               printf("Wrong password\n");
               break;

           case ID_ALREADY_CONNECTED:
               printf("Already connected to this system\n");
               break;

           case ID_CONNECTION_BANNED:
               printf("Banned from server\n");
               break;
       }
   }

Accepting Connections (Server)
------------------------------

To accept incoming connections:

.. code-block:: cpp

   // Must be ≤ maxConnections from Startup()
   peer->SetMaximumIncomingConnections(100);

Optionally set a password:

.. code-block:: cpp

   const char* password = "secretpassword";
   peer->SetIncomingPassword(password, strlen(password));

Handle new connections:

.. code-block:: cpp

   case ID_NEW_INCOMING_CONNECTION:
       printf("New client connected: %s\n",
              packet->systemAddress.ToString());
       break;

Disconnecting
-------------

Graceful disconnect:

.. code-block:: cpp

   peer->CloseConnection(systemAddress, true);  // true = send notification

The remote system receives ``ID_DISCONNECTION_NOTIFICATION``.

Forceful disconnect (no notification):

.. code-block:: cpp

   peer->CloseConnection(systemAddress, false);

The remote system will eventually get ``ID_CONNECTION_LOST``.

Connection Events Summary
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Message ID
     - Description
   * - ``ID_CONNECTION_REQUEST_ACCEPTED``
     - Successfully connected (client)
   * - ``ID_NEW_INCOMING_CONNECTION``
     - New client connected (server)
   * - ``ID_CONNECTION_ATTEMPT_FAILED``
     - Could not connect
   * - ``ID_NO_FREE_INCOMING_CONNECTIONS``
     - Server is full
   * - ``ID_INVALID_PASSWORD``
     - Wrong password
   * - ``ID_DISCONNECTION_NOTIFICATION``
     - Clean disconnect
   * - ``ID_CONNECTION_LOST``
     - Connection timed out
   * - ``ID_CONNECTION_BANNED``
     - IP is banned

Checking Connection State
-------------------------

.. code-block:: cpp

   MafiaNet::ConnectionState state = peer->GetConnectionState(systemAddress);

   switch (state) {
       case MafiaNet::IS_CONNECTED:
           // Fully connected
           break;
       case MafiaNet::IS_CONNECTING:
           // Connection in progress
           break;
       case MafiaNet::IS_DISCONNECTING:
           // Disconnecting
           break;
       case MafiaNet::IS_NOT_CONNECTED:
           // Not connected
           break;
   }

See Also
--------

* :doc:`startup` - Starting MafiaNet
* :doc:`../advanced/debugging-disconnects` - Troubleshooting connection issues
