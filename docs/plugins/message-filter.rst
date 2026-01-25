MessageFilter Plugin
====================

The MessageFilter plugin provides packet filtering and access control based on
message IDs and system addresses. It allows you to restrict which message types
are accepted from specific systems, enhancing security and control.

Basic Usage
-----------

**Setting up message filtering:**

.. code-block:: cpp

   #include "slikenet/MessageFilter.h"

   MafiaNet::MessageFilter messageFilter;
   peer->AttachPlugin(&messageFilter);

   // Define filter channels
   enum FilterChannels {
       CHANNEL_UNAUTHENTICATED = 0,
       CHANNEL_AUTHENTICATED = 1,
       CHANNEL_ADMIN = 2
   };

   // Set allowed messages for unauthenticated users
   messageFilter.SetAllowMessageID(true, ID_USER_PACKET_ENUM,
                                    CHANNEL_UNAUTHENTICATED);
   messageFilter.SetAllowMessageID(true, ID_LOGIN_REQUEST,
                                    CHANNEL_UNAUTHENTICATED);

   // Set allowed messages for authenticated users
   messageFilter.SetAllowMessageID(true, ID_GAME_DATA,
                                    CHANNEL_AUTHENTICATED);
   messageFilter.SetAllowMessageID(true, ID_CHAT_MESSAGE,
                                    CHANNEL_AUTHENTICATED);

   // Admin channel allows everything
   messageFilter.SetAllowMessageID(true, ID_ADMIN_COMMAND,
                                    CHANNEL_ADMIN);

**Assigning systems to channels:**

.. code-block:: cpp

   // New connections start unauthenticated
   void OnNewConnection(MafiaNet::SystemAddress addr) {
       messageFilter.SetSystemFilterSet(addr, CHANNEL_UNAUTHENTICATED);
   }

   // After successful login
   void OnLoginSuccess(MafiaNet::SystemAddress addr, bool isAdmin) {
       if (isAdmin) {
           messageFilter.SetSystemFilterSet(addr, CHANNEL_ADMIN);
       } else {
           messageFilter.SetSystemFilterSet(addr, CHANNEL_AUTHENTICATED);
       }
   }

**Handling filtered messages:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       if (packet->data[0] == ID_RPC_REMOTE_ERROR) {
           // Message was filtered - log or handle
           printf("Filtered message from %s\n",
                  packet->systemAddress.ToString());
       }
       peer->DeallocatePacket(packet);
   }

Key Features
------------

* Channel-based filtering system
* Per-message-ID allow/deny rules
* Dynamic system-to-channel assignment
* Automatic rejection of disallowed messages
* Integration with authentication systems
* Default allow or deny policies

Configuration Options
---------------------

* ``SetAllowMessageID()`` - Allow/deny specific message ID
* ``SetAllowRPC()`` - Filter RPC calls by name
* ``SetSystemFilterSet()`` - Assign system to filter channel
* ``SetActionOnDisallowedMessage()`` - Configure rejection behavior
* ``SetAutoAddNewConnectionsToFilter()`` - Default channel for new systems

See Also
--------

* :doc:`two-way-authentication` - Secure authentication
* :doc:`packet-logger` - Monitor filtered packets
* :doc:`../basics/network-messages` - Message ID system
