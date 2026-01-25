TeamBalancer
============

TeamBalancer automatically balances teams based on configurable rules.

Overview
--------

TeamBalancer works with :doc:`team-manager` to:

- Automatically assign players to teams
- Rebalance teams when players join/leave
- Enforce team size limits
- Support different balancing algorithms

Setup
-----

.. code-block:: cpp

   #include "mafianet/TeamBalancer.h"

   MafiaNet::TeamBalancer* balancer = MafiaNet::TeamBalancer::GetInstance();
   peer->AttachPlugin(balancer);

   // Set the TeamManager instance
   balancer->SetTeamManager(teamManager);

Configuration
-------------

.. code-block:: cpp

   // Set maximum team size difference
   balancer->SetMaxTeamSizeDifference(1);

   // Set balancing algorithm
   balancer->SetBalanceTeamsOnMemberAdd(true);
   balancer->SetBalanceTeamsOnMemberRemove(true);

   // Lock teams (prevent auto-balancing temporarily)
   balancer->SetLockTeams(true);

Balancing Modes
---------------

**Fill First**

Fill one team before starting another:

.. code-block:: cpp

   balancer->SetDefaultBalancingAlgorithm(
       MafiaNet::TeamBalancer::FILL_IN_ORDER);

**Round Robin**

Alternate between teams:

.. code-block:: cpp

   balancer->SetDefaultBalancingAlgorithm(
       MafiaNet::TeamBalancer::ROUND_ROBIN);

**Smallest Team First**

Add to the smallest team:

.. code-block:: cpp

   balancer->SetDefaultBalancingAlgorithm(
       MafiaNet::TeamBalancer::SMALLEST_TEAM);

Manual Balancing
----------------

.. code-block:: cpp

   // Request rebalance
   balancer->RequestRebalance();

   // Move specific player
   balancer->RequestTeamSwitch(playerGuid, newTeamIndex);

Handling Events
---------------

.. code-block:: cpp

   switch (packet->data[0]) {
       case ID_TEAM_BALANCER_TEAM_ASSIGNED: {
           // Player was assigned to a team
           MafiaNet::BitStream bs(packet->data, packet->length, false);
           bs.IgnoreBytes(1);

           MafiaNet::RakNetGUID playerGuid;
           int teamIndex;
           bs.Read(playerGuid);
           bs.Read(teamIndex);

           printf("Player assigned to team %d\n", teamIndex);
           break;
       }
   }

See Also
--------

* :doc:`team-manager` - Team management
* :doc:`overview` - Plugin basics
