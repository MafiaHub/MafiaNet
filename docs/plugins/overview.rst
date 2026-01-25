Plugins Overview
================

MafiaNet's plugin system allows extending functionality without modifying core code.

Using Plugins
-------------

.. code-block:: cpp

   #include "mafianet/PluginInterface2.h"

   // Create plugin instance
   MafiaNet::SomePlugin* plugin = MafiaNet::SomePlugin::GetInstance();

   // Attach to peer
   peer->AttachPlugin(plugin);

   // Later, detach if needed
   peer->DetachPlugin(plugin);

   // Destroy when done
   MafiaNet::SomePlugin::DestroyInstance(plugin);

Available Plugins
-----------------

**Networking**

- :doc:`fully-connected-mesh-2` - Peer-to-peer mesh networking
- :doc:`nat-punchthrough` - NAT traversal
- :doc:`nat-type-detection` - Detect NAT type
- :doc:`router2` - Route packets through peers

**Data Transfer**

- :doc:`file-list-transfer` - Transfer files
- :doc:`directory-delta-transfer` - Sync directories
- :doc:`autopatcher` - Delta patching system

**Game Features**

- :doc:`replica-manager-3` - Object replication
- :doc:`ready-event` - Synchronize ready states
- :doc:`team-manager` - Team management
- :doc:`rpc4` - Remote procedure calls

**Utilities**

- :doc:`message-filter` - Filter messages
- :doc:`packet-logger` - Log network traffic
- :doc:`two-way-authentication` - Secure authentication
- :doc:`cloud-computing` - Distributed architecture

Creating Custom Plugins
-----------------------

Extend ``PluginInterface2``:

.. code-block:: cpp

   class MyPlugin : public MafiaNet::PluginInterface2 {
   public:
       // Called when attached
       virtual void OnAttach() override {
           printf("Plugin attached\n");
       }

       // Process received packets
       virtual MafiaNet::PluginReceiveResult OnReceive(
           MafiaNet::Packet* packet) override {

           if (packet->data[0] == MY_MESSAGE) {
               HandleMyMessage(packet);
               return MafiaNet::RR_STOP_PROCESSING_AND_DEALLOCATE;
           }
           return MafiaNet::RR_CONTINUE_PROCESSING;
       }

       // Connection events
       virtual void OnNewConnection(
           const MafiaNet::SystemAddress& addr,
           MafiaNet::RakNetGUID guid,
           bool isIncoming) override {
           printf("New connection: %s\n", addr.ToString());
       }

       virtual void OnClosedConnection(
           const MafiaNet::SystemAddress& addr,
           MafiaNet::RakNetGUID guid,
           MafiaNet::PI2_LostConnectionReason reason) override {
           printf("Connection closed: %s\n", addr.ToString());
       }
   };

Plugin Receive Results
----------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Result
     - Description
   * - ``RR_CONTINUE_PROCESSING``
     - Pass to next plugin/application
   * - ``RR_STOP_PROCESSING``
     - Stop, but keep packet for application
   * - ``RR_STOP_PROCESSING_AND_DEALLOCATE``
     - Stop and deallocate packet

Plugin Order
------------

Plugins process packets in attachment order. First attached processes first.

.. code-block:: cpp

   peer->AttachPlugin(loggerPlugin);    // Processes first
   peer->AttachPlugin(filterPlugin);    // Processes second
   peer->AttachPlugin(gamePlugin);      // Processes third
