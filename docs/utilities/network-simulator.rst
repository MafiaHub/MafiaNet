Network Simulator
=================

The network simulator allows testing your game under various network conditions.

Overview
--------

Simulate real-world network conditions:

- Packet loss
- Latency (ping)
- Latency variance (jitter)
- Bandwidth limits
- Packet duplication
- Out-of-order delivery

Enabling Simulation
-------------------

.. code-block:: cpp

   // Enable the network simulator
   peer->ApplyNetworkSimulator(
       0.1f,    // 10% packet loss
       100,     // 100ms minimum latency
       50       // 50ms variance (jitter)
   );

Parameters
----------

.. code-block:: cpp

   void ApplyNetworkSimulator(
       float packetloss,        // 0.0 to 1.0 (0% to 100%)
       unsigned short minExtraPing,  // Minimum added latency (ms)
       unsigned short extraPingVariance  // Random variance (ms)
   );

Example Configurations
----------------------

**Good Connection (LAN)**

.. code-block:: cpp

   peer->ApplyNetworkSimulator(0.0f, 5, 2);

**Average Broadband**

.. code-block:: cpp

   peer->ApplyNetworkSimulator(0.01f, 50, 20);

**Poor Mobile Connection**

.. code-block:: cpp

   peer->ApplyNetworkSimulator(0.05f, 150, 100);

**Terrible Connection (Stress Test)**

.. code-block:: cpp

   peer->ApplyNetworkSimulator(0.2f, 500, 200);

Disabling Simulation
--------------------

.. code-block:: cpp

   // Disable by setting all values to 0
   peer->ApplyNetworkSimulator(0.0f, 0, 0);

   // Or check if enabled
   bool isEnabled = peer->IsNetworkSimulatorActive();

Testing Strategies
------------------

1. **Test with varying conditions** - Change parameters during gameplay
2. **Test edge cases** - Very high latency, high packet loss
3. **Test recovery** - Network improves/degrades suddenly
4. **Test all reliability types** - Ensure UNRELIABLE and RELIABLE both work

Build Configuration
-------------------

Network simulator is only available in debug builds by default. To enable in release:

.. code-block:: cpp

   // In RakNetDefinesOverrides.h
   #define _DEBUG_NETWORK_SIMULATOR 1

See Also
--------

* :doc:`../basics/reliability-types` - How reliability handles loss
* :doc:`../basics/statistics` - Monitor actual network conditions
