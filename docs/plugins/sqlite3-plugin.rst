SQLite3Plugin
=============

SQLite3Plugin enables networked SQLite database operations.

Overview
--------

Execute SQL statements over the network:

- Query remote databases
- Synchronized data storage
- Logging and analytics
- Persistent game data

Server Setup
------------

.. code-block:: cpp

   #include "mafianet/sqlite3plugin/SQLite3ServerPlugin.h"

   MafiaNet::SQLite3ServerPlugin* sqlServer =
       MafiaNet::SQLite3ServerPlugin::GetInstance();
   peer->AttachPlugin(sqlServer);

   // Add a database
   sqlServer->AddDBHandle("gamedata", "gamedata.db");

Client Setup
------------

.. code-block:: cpp

   #include "mafianet/sqlite3plugin/SQLite3ClientPlugin.h"

   MafiaNet::SQLite3ClientPlugin* sqlClient =
       MafiaNet::SQLite3ClientPlugin::GetInstance();
   peer->AttachPlugin(sqlClient);

Executing Queries
-----------------

.. code-block:: cpp

   // Execute a query
   sqlClient->_sqlite3_exec(
       "gamedata",                              // Database identifier
       "INSERT INTO scores (name, score) VALUES ('Player1', 100)",
       HIGH_PRIORITY,
       RELIABLE_ORDERED,
       0,
       serverAddress
   );

   // Query with results
   sqlClient->_sqlite3_exec(
       "gamedata",
       "SELECT * FROM scores ORDER BY score DESC LIMIT 10",
       HIGH_PRIORITY,
       RELIABLE_ORDERED,
       0,
       serverAddress
   );

Handling Results
----------------

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_SQLite3_EXEC: {
           MafiaNet::BitStream bs(packet->data, packet->length, false);
           bs.IgnoreBytes(1);

           // Read result
           MafiaNet::RakString dbIdentifier;
           MafiaNet::RakString inputStatement;
           bool isRequest;

           bs.Read(dbIdentifier);
           bs.Read(inputStatement);
           bs.Read(isRequest);

           if (!isRequest) {
               // This is a response
               int numColumns, numRows;
               bs.Read(numColumns);
               bs.Read(numRows);

               for (int row = 0; row < numRows; row++) {
                   for (int col = 0; col < numColumns; col++) {
                       MafiaNet::RakString value;
                       bs.Read(value);
                       printf("%s ", value.C_String());
                   }
                   printf("\n");
               }
           }
           break;
       }
   }

SQLite3LoggerPlugin
-------------------

For structured logging:

.. code-block:: cpp

   #include "mafianet/sqlite3plugin/Logger/SQLiteServerLoggerPlugin.h"

   MafiaNet::SQLiteServerLoggerPlugin* logger =
       MafiaNet::SQLiteServerLoggerPlugin::GetInstance();
   peer->AttachPlugin(logger);

   // Clients can now send log entries
   // See Samples/SQLite3Plugin/ for details

Security Considerations
-----------------------

- Validate all SQL inputs
- Use parameterized queries when possible
- Restrict database permissions
- Consider read-only access for clients

See Also
--------

* :doc:`overview` - Plugin basics
* :doc:`../getting-started/samples` - Sample applications
