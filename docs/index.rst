MafiaNet Documentation
======================

**MafiaNet** is a cross-platform networking engine for multiplayer games. Built on the foundation of RakNet and SLikeNet, it provides reliable UDP messaging, NAT traversal, peer-to-peer networking, and various game-specific networking features.

**Supported Platforms:** Windows, Linux, macOS (primary) | iOS, Android (limited)

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   getting-started/installation
   getting-started/quickstart
   getting-started/building
   getting-started/tutorial
   getting-started/system-overview
   getting-started/multiplayer-components
   getting-started/samples

.. toctree::
   :maxdepth: 2
   :caption: The Basics

   basics/startup
   basics/connecting
   basics/creating-packets
   basics/sending-packets
   basics/receiving-packets
   basics/bitstreams
   basics/reliability-types
   basics/network-messages
   basics/system-address
   basics/timestamping
   basics/network-id-object
   basics/statistics
   basics/secure-connections
   basics/data-compression

.. toctree::
   :maxdepth: 2
   :caption: Architecture Guide

   guide/concepts
   guide/client-server
   guide/peer-to-peer
   guide/plugins

.. toctree::
   :maxdepth: 2
   :caption: Plugins

   plugins/overview
   plugins/replica-manager-3
   plugins/rpc4
   plugins/fully-connected-mesh-2
   plugins/nat-punchthrough
   plugins/nat-type-detection
   plugins/file-list-transfer
   plugins/directory-delta-transfer
   plugins/autopatcher
   plugins/ready-event
   plugins/team-manager
   plugins/router2
   plugins/message-filter
   plugins/packet-logger
   plugins/two-way-authentication
   plugins/cloud-computing
   plugins/lobby2
   plugins/rakvoice
   plugins/team-balancer
   plugins/sqlite3-plugin
   plugins/steam-lobby

.. toctree::
   :maxdepth: 2
   :caption: Advanced Topics

   advanced/nat-traversal-architecture
   advanced/custom-memory-management
   advanced/preprocessor-directives
   advanced/ipv6-support
   advanced/congestion-control
   advanced/debugging-disconnects

.. toctree::
   :maxdepth: 2
   :caption: Utilities

   utilities/tcp-interface
   utilities/console-server
   utilities/email-sender
   utilities/string-compressor
   utilities/crash-reporter
   utilities/network-simulator

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api/core
   api/plugins
   api/data-structures

.. toctree::
   :maxdepth: 1
   :caption: Support

   support/faq
   support/programming-tips
   contributing
   changelog


Quick Links
-----------

* :ref:`genindex`
* :ref:`search`


Features
--------

* **Reliable UDP** - Ordered, sequenced, and reliable message delivery
* **NAT Traversal** - Built-in NAT punchthrough for peer-to-peer connections
* **Cross-Platform** - Windows, Linux, macOS support
* **Plugin System** - Extensible architecture with modular plugins
* **Security** - Secure handshake and encryption support
* **Replication** - Object replication system for game state synchronization
* **Voice Chat** - RakVoice for in-game voice communication
* **Autopatcher** - Delta patching system for game updates


License
-------

MafiaNet is released under the MIT License. See the LICENSE file for details.
