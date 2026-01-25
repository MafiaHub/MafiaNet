Autopatcher Plugin
==================

The Autopatcher system provides a complete delta patching solution for game updates.
It computes binary differences between file versions and transfers only the changes,
significantly reducing patch download sizes.

Basic Usage
-----------

**Server setup:**

.. code-block:: cpp

   #include "slikenet/AutopatcherServer.h"
   #include "slikenet/AutopatcherRepositoryInterface.h"

   // Use file-based or database repository
   MafiaNet::AutopatcherServer autopatcherServer;
   MafiaNet::FileListTransfer flt;

   peer->AttachPlugin(&autopatcherServer);
   peer->AttachPlugin(&flt);

   autopatcherServer.SetFileListTransferPlugin(&flt);
   autopatcherServer.SetRepository(&repository);

**Client requesting patches:**

.. code-block:: cpp

   #include "slikenet/AutopatcherClient.h"

   class MyPatchCallback : public MafiaNet::AutopatcherClientCBInterface {
   public:
       void OnPatchProgress(double progress) override {
           printf("Patching: %.1f%%\n", progress * 100.0);
       }

       void OnPatchComplete() override {
           printf("Patching complete!\n");
       }

       void OnPatchFailed(const char* error) override {
           printf("Patch failed: %s\n", error);
       }
   };

   MafiaNet::AutopatcherClient autopatcherClient;
   peer->AttachPlugin(&autopatcherClient);

   MyPatchCallback callback;
   autopatcherClient.SetCallbackInterface(&callback);

   // Request patches for application
   autopatcherClient.PatchApplication("MyGame", "./game_directory",
                                       serverAddress, &flt);

**Creating patch data:**

.. code-block:: cpp

   // Add a new version to the repository
   repository.AddNewVersion("./new_version_files", "1.2.0");

   // Generate delta patches from previous versions
   repository.CreatePatch("1.1.0", "1.2.0");

Key Features
------------

* Binary delta patching (only transfers changes)
* Multi-version support with automatic path finding
* Database or file-based repository backends
* Progress callbacks for UI integration
* Integrity verification via checksums
* Resumable downloads after interruption
* Rollback support for failed patches

Repository Backends
-------------------

* ``AutopatcherRepositoryInterface`` - Base interface
* ``AutopatcherMySQLRepository`` - MySQL storage
* ``AutopatcherPostgreSQLRepository`` - PostgreSQL storage

See Also
--------

* :doc:`file-list-transfer` - File transfer system
* :doc:`directory-delta-transfer` - Directory sync
* :doc:`../getting-started/samples` - Autopatcher sample
