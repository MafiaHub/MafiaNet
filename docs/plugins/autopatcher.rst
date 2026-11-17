Autopatcher Plugin
==================

The Autopatcher system provides a complete delta patching solution for game updates.
It computes binary differences between file versions and transfers only the changes,
significantly reducing patch download sizes.

Basic Usage
-----------

**Server setup:**

Supply an implementation of ``MafiaNet::AutopatcherRepositoryInterface`` that
lists changed files and serves patches for your application. Pass at least one
repository instance per worker thread to ``StartThreads()`` and keep the
instances alive while the server uses them.

.. code-block:: cpp

   #include "AutopatcherServer.h"
   #include "mafianet/autopatcher_repository_interface.h"

   MafiaNet::AutopatcherServer autopatcherServer;
   MafiaNet::FileListTransfer flt;

   peer->AttachPlugin(&autopatcherServer);
   peer->AttachPlugin(&flt);

   autopatcherServer.SetFileListTransferPlugin(&flt);
   MafiaNet::AutopatcherRepositoryInterface* repositories[] = {&repository};
   autopatcherServer.StartThreads(1, 1, repositories);

**Client requesting patches:**

.. code-block:: cpp

   #include "AutopatcherClient.h"

   // Use the connected peer, its attached file-transfer plugin, and the
   // application's AutopatcherClientCBInterface implementation.
   peer->AttachPlugin(&autopatcherClient);
   autopatcherClient.SetFileListTransferPlugin(&flt);
   autopatcherClient.PatchApplication("MyGame", "./game_directory",
                                      lastUpdateDate, serverAddress, &callback,
                                      restartOutputFilename, pathToRestartExe);

Incremental patch checks
------------------------

Pass the last successful patch's server timestamp as ``lastUpdateDate`` to
``AutopatcherClient::PatchApplication()``. The server uses it to look up files
that changed since that update. After receiving ``ID_AUTOPATCHER_FINISHED``,
save ``AutopatcherClient::GetServerDate()`` with that installation for its next
check. If the patch requires a restart, advance the saved timestamp after the
replacement files have been installed.

For a fresh distribution, bundle the server timestamp recorded when its files
were published. Pass zero if the installed version is unknown or you want a
full scan to repair the installation.

Key Features
------------

* Binary delta patching (only transfers changes)
* Multi-version support with automatic path finding
* Progress callbacks for UI integration
* Integrity verification via checksums
* Resumable downloads after interruption
* Rollback support for failed patches

See Also
--------

* :doc:`file-list-transfer` - File transfer system
* :doc:`directory-delta-transfer` - Directory sync
* :doc:`../getting-started/samples` - Autopatcher sample
