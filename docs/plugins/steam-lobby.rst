Steam Lobby Integration
=======================

MafiaNet can integrate with Steam's lobby and matchmaking system.

Overview
--------

The Steam lobby integration provides:

- Steam matchmaking integration
- Steam friends list access
- Steam authentication
- NAT traversal via Steam relay

Requirements
------------

- Steamworks SDK
- Steam client running
- Valid Steam App ID

Setup
-----

.. code-block:: cpp

   #include "mafianet/Lobby2/Steam/Lobby2Client_Steam.h"

   // Initialize Steam
   if (!SteamAPI_Init()) {
       printf("Steam not running\n");
       return;
   }

   MafiaNet::Lobby2Client_Steam* steamLobby =
       MafiaNet::Lobby2Client_Steam::GetInstance();
   peer->AttachPlugin(steamLobby);

Creating a Lobby
----------------

.. code-block:: cpp

   // Create a public lobby
   steamLobby->CreateLobby(
       k_ELobbyTypePublic,  // Visibility
       4                     // Max players
   );

   // Handle result
   case ID_LOBBY_GENERAL: {
       MafiaNet::Lobby2Message* msg =
           steamLobby->GetMessageFromPacket(packet);

       if (msg->resultCode == MafiaNet::REC_SUCCESS) {
           printf("Lobby created\n");
       }
       break;
   }

Joining a Lobby
---------------

.. code-block:: cpp

   // Search for lobbies
   steamLobby->SearchLobbies();

   // Join a specific lobby
   steamLobby->JoinLobby(lobbyId);

Lobby Data
----------

.. code-block:: cpp

   // Set lobby data (host only)
   steamLobby->SetLobbyData("map", "dm_arena");
   steamLobby->SetLobbyData("gamemode", "deathmatch");

   // Get lobby data
   const char* map = steamLobby->GetLobbyData("map");

   // Set member data
   steamLobby->SetLobbyMemberData("ready", "1");

Connecting Players
------------------

Once in a lobby, connect players directly:

.. code-block:: cpp

   // Get other player's Steam ID
   CSteamID otherPlayer = GetLobbyMember(lobbyId, memberIndex);

   // Connect via Steam networking
   peer->Connect(otherPlayer.IsValid() ?
       steamLobby->GetAddressFromSteamID(otherPlayer) : nullptr);

Steam Relay (NAT Traversal)
---------------------------

Steam provides relay servers when direct connection fails:

.. code-block:: cpp

   // Enable Steam relay as fallback
   steamLobby->SetUseRelay(true);

See Also
--------

* :doc:`lobby2` - General lobby system
* :doc:`nat-punchthrough` - Alternative NAT traversal
* :doc:`overview` - Plugin basics
