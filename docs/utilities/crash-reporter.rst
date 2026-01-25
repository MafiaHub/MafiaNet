CrashReporter
=============

The CrashReporter utility captures crash information and can automatically send
reports to a server for analysis. This helps identify and fix issues in
deployed game clients.

Basic Usage
-----------

**Initializing crash reporting:**

.. code-block:: cpp

   #include "slikenet/CrashReporter.h"

   int main() {
       // Initialize crash handler early
       MafiaNet::CrashReporter::Start("crash_reports",  // Local directory
                                       "MyGame",         // Application name
                                       "1.0.0");         // Version

       // Set crash report server (optional)
       MafiaNet::CrashReporter::SetReportServer(
           "crashes.example.com", 8080);

       // Your game code
       RunGame();

       // Cleanup
       MafiaNet::CrashReporter::Stop();
       return 0;
   }

**Adding custom crash data:**

.. code-block:: cpp

   // Add context that will be included in crash reports
   void OnPlayerJoin(const char* playerName, int playerId) {
       char context[256];
       sprintf(context, "Player: %s (ID: %d)", playerName, playerId);
       MafiaNet::CrashReporter::AddContext("CurrentPlayer", context);
   }

   void OnMapLoad(const char* mapName) {
       MafiaNet::CrashReporter::AddContext("CurrentMap", mapName);
   }

   void OnGameState(GameState state) {
       MafiaNet::CrashReporter::AddContext("GameState",
                                           GameStateToString(state));
   }

**Server-side report handling:**

.. code-block:: cpp

   #include "slikenet/TCPInterface.h"

   // Simple crash report receiver
   void RunCrashServer() {
       MafiaNet::TCPInterface tcp;
       tcp.Start(8080, 32);

       while (running) {
           MafiaNet::Packet* packet = tcp.Receive();
           if (packet) {
               // Parse crash report
               SaveCrashReport(packet->data, packet->length,
                              packet->systemAddress.ToString());
               tcp.DeallocatePacket(packet);
           }
       }
   }

**Manual crash report generation:**

.. code-block:: cpp

   // Generate report without crashing (for error logging)
   void OnCriticalError(const char* error) {
       MafiaNet::CrashReporter::GenerateReport(error, false);

       // Optionally send immediately
       MafiaNet::CrashReporter::SendPendingReports();
   }

Key Features
------------

* Automatic crash detection (signals/exceptions)
* Stack trace capture
* Custom context data
* Automatic server upload
* Local report storage
* Cross-platform support

Report Contents
---------------

Crash reports typically include:

* Exception type and message
* Stack trace (if available)
* Application name and version
* OS and hardware information
* Custom context data
* Timestamp

Configuration Options
---------------------

* ``Start()`` - Initialize crash handling
* ``Stop()`` - Cleanup handlers
* ``SetReportServer()`` - Configure upload server
* ``AddContext()`` - Add custom data
* ``GenerateReport()`` - Manual report generation
* ``SendPendingReports()`` - Upload stored reports

Platform Notes
--------------

* **Windows**: Uses SEH and MiniDump
* **Linux/macOS**: Uses signal handlers
* Stack traces require debug symbols

See Also
--------

* :doc:`tcp-interface` - Report transmission
* :doc:`../advanced/debugging-disconnects` - Network debugging
* :doc:`../plugins/packet-logger` - Network logging
