TeamManager Plugin
==================

The TeamManager plugin handles team assignment and balancing in multiplayer games.
It synchronizes team membership across all connected systems and enforces team
size limits and balancing rules.

Basic Usage
-----------

**Setting up teams:**

.. code-block:: cpp

   #include "slikenet/TeamManager.h"

   MafiaNet::TeamManager teamManager;
   peer->AttachPlugin(&teamManager);

   // Define teams
   MafiaNet::TM_Team blueTeam, redTeam, spectators;

   teamManager.AddTeam(&blueTeam);
   teamManager.AddTeam(&redTeam);
   teamManager.AddTeam(&spectators);

   // Set team limits
   blueTeam.SetMemberLimit(8);
   redTeam.SetMemberLimit(8);
   spectators.SetMemberLimit(0);  // No limit

   // Enable auto-balancing
   teamManager.SetAutoBalanceTeams(true);

**Managing team membership:**

.. code-block:: cpp

   // Create team member for local player
   MafiaNet::TM_TeamMember* myMember = teamManager.CreateTeamMember();
   myMember->SetGuid(peer->GetMyGUID());

   // Request to join a team
   teamManager.RequestTeamSwitch(myMember, &blueTeam);

   // Or assign directly (host only)
   if (teamManager.GetHost() == peer->GetMyGUID()) {
       teamManager.SetTeam(player, &redTeam);
   }

**Handling team events:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       switch (packet->data[0]) {
           case ID_TEAM_BALANCER_TEAM_ASSIGNED:
               printf("Assigned to team\n");
               break;

           case ID_TEAM_BALANCER_REQUESTED_TEAM_FULL:
               printf("Requested team is full\n");
               break;

           case ID_TEAM_BALANCER_INTERNAL:
               teamManager.DecodeTeamAssigned(packet);
               break;
       }
       peer->DeallocatePacket(packet);
   }

Key Features
------------

* Synchronized team state across all peers
* Configurable team size limits
* Automatic team balancing
* Host-based authority model
* Team switch requests and approval
* Support for multiple teams per player
* Spectator team support

Configuration Options
---------------------

* ``AddTeam()`` - Register a new team
* ``RemoveTeam()`` - Remove a team
* ``CreateTeamMember()`` - Create player representation
* ``RequestTeamSwitch()`` - Request team change
* ``SetAutoBalanceTeams()`` - Enable/disable balancing
* ``SetHost()`` - Set authoritative host

See Also
--------

* :doc:`ready-event` - Team-based ready coordination
* :doc:`lobby2` - Pre-game team selection
* :doc:`fully-connected-mesh-2` - P2P team synchronization
