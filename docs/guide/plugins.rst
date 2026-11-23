Plugins
=======

MafiaNet's plugin system allows extending functionality without modifying core code.

Using Plugins
-------------

Plugins are attached to a ``RakPeerInterface``:

.. code-block:: cpp

   MafiaNet::SomePlugin* plugin = MafiaNet::SomePlugin::GetInstance();
   peer->AttachPlugin(plugin);

   // Later, to remove
   peer->DetachPlugin(plugin);
   MafiaNet::SomePlugin::DestroyInstance(plugin);

Available Plugins
-----------------

**Networking Plugins**

* ``FullyConnectedMesh2`` - Maintains fully connected peer networks
* ``NatPunchthroughClient/Server`` - NAT traversal support
* ``Router2`` - Route packets through intermediate peers
* ``UDPProxyClient/Server/Coordinator`` - UDP relay when NAT fails

**Data Transfer Plugins**

* ``FileListTransfer`` - Transfer files with compression and delta updates
* ``DirectoryDeltaTransfer`` - Sync directory contents between peers

**Game Plugins**

* ``ReadyEvent`` - Synchronize ready state across peers

**Utility Plugins**

* ``MessageFilter`` - Filter messages by type
* ``PacketLogger`` - Log all network traffic

Creating Custom Plugins
-----------------------

Extend ``PluginInterface2``:

.. code-block:: cpp

   class MyPlugin : public MafiaNet::PluginInterface2 {
   public:
       virtual MafiaNet::PluginReceiveResult OnReceive(
           MafiaNet::Packet* packet) override {
           // Process packets before they reach the application
           if (packet->data[0] == MY_CUSTOM_MESSAGE) {
               HandleCustomMessage(packet);
               return MafiaNet::RR_STOP_PROCESSING_AND_DEALLOCATE;
           }
           return MafiaNet::RR_CONTINUE_PROCESSING;
       }

       virtual void OnNewConnection(
           const MafiaNet::SystemAddress& address,
           MafiaNet::RakNetGUID guid,
           bool isIncoming) override {
           // Handle new connections
       }

       virtual void OnClosedConnection(
           const MafiaNet::SystemAddress& address,
           MafiaNet::RakNetGUID guid,
           MafiaNet::PI2_LostConnectionReason reason) override {
           // Handle disconnections
       }
   };
