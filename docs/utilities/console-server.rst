ConsoleServer
=============

The ConsoleServer utility provides a telnet-style interface for remote server
administration. Administrators can connect via TCP and execute commands on the
game server.

Basic Usage
-----------

**Setting up the console server:**

.. code-block:: cpp

   #include "slikenet/ConsoleServer.h"
   #include "slikenet/TCPInterface.h"

   MafiaNet::TCPInterface tcp;
   MafiaNet::ConsoleServer console;

   // Start TCP listener
   tcp.Start(9999, 4);  // Port 9999, max 4 admin connections

   // Set transport
   console.SetTransportProvider(&tcp);

   // Add command handlers
   console.AddCommandParser("status", &statusHandler);
   console.AddCommandParser("kick", &kickHandler);
   console.AddCommandParser("ban", &banHandler);

**Creating command handlers:**

.. code-block:: cpp

   class StatusHandler : public MafiaNet::CommandParserInterface {
   public:
       const char* GetName() const override { return "status"; }

       const char* GetHelp() const override {
           return "Shows server status";
       }

       void OnCommand(const char* command, unsigned numParams,
                      char** params,
                      MafiaNet::TransportInterface* transport,
                      const MafiaNet::SystemAddress& addr,
                      const char* originalString) override {
           char response[256];
           sprintf(response, "Players: %d/%d\nUptime: %d seconds\n",
                   playerCount, maxPlayers, uptime);
           transport->Send(addr, response);
       }
   };

   StatusHandler statusHandler;

**Processing in game loop:**

.. code-block:: cpp

   while (gameRunning) {
       // Process console commands
       console.Update();

       // Your game logic
       UpdateGame();
   }

**Connecting as admin:**

.. code-block:: bash

   # Using telnet
   telnet gameserver.example.com 9999

   # Using netcat
   nc gameserver.example.com 9999

   # Then type commands
   > status
   Players: 12/32
   Uptime: 3600 seconds

   > kick player123
   Player kicked.

Key Features
------------

* Telnet-compatible protocol
* Password authentication support
* Custom command registration
* Multiple admin connections
* Command history (client-side)
* Help system for commands

Built-in Commands
-----------------

* ``help`` - List available commands
* ``quit`` - Disconnect from console
* ``password <pass>`` - Authenticate (if required)

Configuration Options
---------------------

* ``SetTransportProvider()`` - Set TCP transport
* ``AddCommandParser()`` - Register command handler
* ``RemoveCommandParser()`` - Unregister command
* ``SetPassword()`` - Require authentication
* ``SetPrompt()`` - Customize command prompt

See Also
--------

* :doc:`tcp-interface` - TCP transport
* :doc:`../plugins/packet-logger` - Network logging
* :doc:`../advanced/debugging-disconnects` - Server diagnostics
