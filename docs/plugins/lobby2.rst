Lobby2 Plugin
=============

The Lobby2 plugin provides a complete matchmaking and lobby system with user
accounts, friends lists, rooms, and game sessions. It supports both server-hosted
and peer-to-peer architectures.

Basic Usage
-----------

**Client setup:**

.. code-block:: cpp

   #include "slikenet/Lobby2Client.h"

   class MyLobbyCallbacks : public MafiaNet::Lobby2Callbacks {
   public:
       void MessageResult(MafiaNet::Lobby2Message* msg) override {
           switch (msg->resultCode) {
               case MafiaNet::L2RC_SUCCESS:
                   HandleSuccess(msg);
                   break;
               default:
                   printf("Lobby error: %s\n",
                          msg->GetResultCodeDescription());
                   break;
           }
       }
   };

   MafiaNet::Lobby2Client lobby2Client;
   MyLobbyCallbacks callbacks;
   lobby2Client.SetCallbackInterface(&callbacks);
   peer->AttachPlugin(&lobby2Client);

**User registration and login:**

.. code-block:: cpp

   // Register new user
   MafiaNet::Client_RegisterAccount* reg =
       new MafiaNet::Client_RegisterAccount();
   reg->userName = "player123";
   reg->password = "securePassword";
   lobby2Client.SendMsg(reg);

   // Login
   MafiaNet::Client_Login* login = new MafiaNet::Client_Login();
   login->userName = "player123";
   login->password = "securePassword";
   lobby2Client.SendMsg(login);

**Room management:**

.. code-block:: cpp

   // Create a room
   MafiaNet::Client_CreateRoom* create =
       new MafiaNet::Client_CreateRoom();
   create->roomName = "My Game Room";
   create->roomIsPublic = true;
   create->maxSlots = 8;
   lobby2Client.SendMsg(create);

   // Join existing room
   MafiaNet::Client_JoinRoom* join = new MafiaNet::Client_JoinRoom();
   join->roomId = targetRoomId;
   lobby2Client.SendMsg(join);

   // Search for rooms
   MafiaNet::Client_SearchRooms* search =
       new MafiaNet::Client_SearchRooms();
   search->gameIdentifier = "MyGame";
   lobby2Client.SendMsg(search);

Key Features
------------

* User account management (register, login, profile)
* Friends list with presence
* Room creation and searching
* Matchmaking queues
* Clan/guild support
* Ranking and leaderboards
* Chat integration
* Database backend support

Message Types
-------------

* ``Client_RegisterAccount`` - Create new account
* ``Client_Login`` / ``Client_Logout`` - Session management
* ``Client_CreateRoom`` / ``Client_JoinRoom`` - Room management
* ``Client_SearchRooms`` - Find available rooms
* ``Client_AddFriend`` - Social features
* ``Client_StartGame`` - Launch game session

See Also
--------

* :doc:`ready-event` - Synchronize game start
* :doc:`team-manager` - Pre-game team selection
* :doc:`cloud-computing` - Server discovery
