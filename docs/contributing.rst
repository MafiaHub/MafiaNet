Contributing
============

We welcome contributions to MafiaNet! This guide will help you get started.

Getting Started
---------------

1. Fork the repository on GitHub
2. Clone your fork locally
3. Create a branch for your changes

.. code-block:: bash

   git clone https://github.com/YOUR_USERNAME/MafiaNet.git
   cd MafiaNet
   git checkout -b feature/my-new-feature

Code Style
----------

* Use C++17 features where appropriate
* Follow existing code formatting conventions
* Add comments for complex logic
* Keep functions focused and reasonably sized

Building and Testing
--------------------

All tests are GoogleTest, built with ``MAFIANET_BUILD_TESTS=ON`` (requires
``MAFIANET_BUILD_STATIC=ON``) and driven by CTest:

.. code-block:: bash

   cmake -B build -DMAFIANET_BUILD_TESTS=ON
   cmake --build build

   ctest --test-dir build --output-on-failure   # everything
   ctest --test-dir build -L unit               # hermetic unit suite only
   ctest --test-dir build -L integration        # loopback integration suite only
   ctest --test-dir build -R "DispatcherLive"   # by name pattern

Two binaries live under ``Tests/``:

* **UnitTests** (``Tests/Unit/``, label ``unit``): hermetic and deterministic —
  no loopback networking, no wall-clock timing. A started-but-unconnected peer
  with synchronous assertions is acceptable. New tests for pure logic go here.
* **IntegrationTests** (``Tests/Integration/``, label ``integration``): real
  UDP over loopback. Discovered tests run serially (several bind fixed ports)
  with a generous timeout. Shared polling/connect helpers live in
  ``Tests/Support/`` (``CommonFunctions.h``, ``TestHelpers.h``) — extend those
  rather than duplicating polling loops.

New ``.cpp`` files under ``Tests/Unit/`` and ``Tests/Integration/`` are picked
up automatically by the build; no CMake edits are needed unless you add a new
directory. For debugging, run a binary directly with a filter:

.. code-block:: bash

   ./build/Tests/IntegrationTests --gtest_filter='DispatcherLive.*'
   # reproduce flakes:
   ./build/Tests/IntegrationTests --gtest_filter='<Suite>.*' --gtest_repeat=100 --gtest_break_on_failure

.. important::
   Under ctest, every ``TEST()`` runs in its own process — never write tests
   that depend on state from a previous ``TEST()``. Long sequential networking
   scenarios belong in a single ``TEST_F``. In integration tests, use
   OS-assigned ephemeral ports (``SocketDescriptor(0, "127.0.0.1")``, then
   ``peer->GetInternalID().GetPort()``), always poll for a condition with a
   deadline instead of asserting right after a state change, never use a bare
   ``RakSleep(N)`` as a synchronization primitive, and make cleanup survive a
   failed ``ASSERT_`` (fixture ``TearDown()`` or the RAII ``Peer`` /
   ``PacketPtr`` handles).

How to test a new feature
-------------------------

Follow test-driven development: **write the test first, watch it fail for the
right reason, then write the minimal code to make it pass.** A test that passes
the moment you write it has proven nothing.

Test at **two levels** — both are required for anything that touches networking
or replication:

1. **Unit level (deterministic).** Exercise the feature's logic directly with no
   sockets, so it is fast and never flaky — a ``TEST()`` in ``Tests/Unit/``.
   ``Tests/Unit/VirtualWorldTests.cpp`` constructs replicas/connections on the
   stack and asserts the exact return values of the query hooks. This pins down
   *what each decision should be*. Split independent checks into separate
   ``TEST()`` cases with descriptive names, so each is reported and filtered
   individually.

2. **End-to-end / integration (real wire).** Stand up real peers (a server and
   one or more clients) over loopback and pump them until the expected state
   converges — a ``TEST_F`` in ``Tests/Integration/``. Use **condition-based
   waiting** (pump until the condition holds or a generous ceiling is reached),
   never a fixed ``sleep``, and keep phases that share connection state in a
   single ``TEST_F``. Before calling a new integration test done, prove it is
   not flaky with ``--gtest_repeat=10 --gtest_break_on_failure``.

.. warning::

   **Unit tests alone are not enough for replication/networking features.**
   Many bugs only appear in a real topology, because plugins such as
   ReplicaManager3 run the same callbacks on *every* peer (server, clients,
   peers), and the interesting failures live in how those sides interact over
   the wire. The virtual world feature, for example, passed every unit test but
   had a bug where a *client* told the *server* to delete an object — something
   only a real client/server run could surface. If your feature sends anything
   over the network, you must exercise it end-to-end before it is "done".

When something only fails over the wire, instrument the message boundary rather
than guessing: temporary, env-var-guarded prints in the send path
(``Connection_RM3::SendConstruction``) and the receive path
(``ReplicaManager3::OnConstruction`` / ``OnSerialize``), keyed on the relevant
NetworkID, will show exactly which peer did what. Remove the instrumentation
before committing.

ReplicaManager3: respect the authority model
--------------------------------------------

Replica3 callbacks (``QueryConstruction``, ``QueryDestruction``,
``QuerySerialization``) run on **every** system that holds the replica — the
owner *and* every system that downloaded a copy. A copy on a non-authority must
**defer to the topology default** (``QueryConstruction_ServerConstruction``,
``QueryConstruction_ClientConstruction``, ``QueryConstruction_PeerToPeer``) and
must not send construction or destruction *upstream*.

A reliable test for "is this system the authority for sending this entity to this
connection?" is whether its within-topology construction decision is
``RM3CS_SEND_CONSTRUCTION``. Only the authority should make per-connection
visibility decisions (such as hiding an object); a downloaded copy that applies
such logic can, for instance, tell the owner to destroy the object. Gate any
custom visibility/scoping logic on authority, and add a unit test for the
non-authority case.

Gotchas when running several peers in one process
-------------------------------------------------

Self-contained tests and samples often create multiple peers in a single
process. Two collisions to be aware of:

* **GUID collision.** On POSIX a peer's GUID is seeded from the microsecond
  clock at construction, so peers created back-to-back can get identical GUIDs
  and a server will drop the duplicate connection. Space out the
  ``GetInstance()`` calls (e.g. a short ``RakSleep`` between them) so each GUID
  is distinct.
* **NetworkID collision.** Each ``NetworkIDManager`` assigns ids from the same
  base, and ``NetworkID`` carries no creator information, so two independent
  creators can mint the same id (the second is rejected as a duplicate on the
  receiver). Use a single authoritative creator, or otherwise guarantee globally
  unique ids.

Releasing
---------

Version bumps touch several files that must stay in sync. The full procedure is
documented in ``CLAUDE.md`` (the "Releasing" section) — follow it exactly when
cutting a release.

Submitting Changes
------------------

1. Ensure all tests pass
2. Commit your changes with a clear message
3. Push to your fork
4. Open a Pull Request

License
-------

By contributing, you agree that your contributions will be licensed under the MIT License.
