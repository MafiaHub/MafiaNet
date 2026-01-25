DirectoryDeltaTransfer Plugin
=============================

The DirectoryDeltaTransfer plugin synchronizes directory contents between systems
by only transferring files that differ. It computes file hashes to identify changes
and transfers only modified or new files.

Basic Usage
-----------

**Server-side (file host):**

.. code-block:: cpp

   #include "slikenet/DirectoryDeltaTransfer.h"

   MafiaNet::DirectoryDeltaTransfer ddt;
   MafiaNet::FileListTransfer flt;
   peer->AttachPlugin(&ddt);
   peer->AttachPlugin(&flt);

   // Set the directory to serve
   ddt.SetFileListTransferPlugin(&flt);
   ddt.SetApplicationDirectory("./game_assets");

   // Add subdirectories to sync
   ddt.AddUploadsFromSubdirectory("textures");
   ddt.AddUploadsFromSubdirectory("models");
   ddt.AddUploadsFromSubdirectory("sounds");

**Client-side (receiver):**

.. code-block:: cpp

   class MySyncCallback : public MafiaNet::FileListTransferCBInterface {
   public:
       bool OnFile(OnFileStruct* ofs) override {
           printf("Synced: %s (%d bytes)\n",
                  ofs->fileName, ofs->byteLengthOfThisFile);
           return true;
       }

       void OnFileProgress(FileProgressStruct* fps) override {
           float percent = (float)fps->partCount /
                          (float)fps->partTotal * 100.0f;
           printf("Downloading: %.1f%%\n", percent);
       }
   };

   MySyncCallback callback;
   ddt.SetFileListTransferPlugin(&flt);
   ddt.SetApplicationDirectory("./local_assets");

   // Request sync from server
   ddt.DownloadFromSubdirectory("textures", "./local_assets/textures",
                                 true, serverAddress, &callback,
                                 HIGH_PRIORITY, 0, false);

Key Features
------------

* Hash-based file comparison (only transfers changed files)
* Recursive directory synchronization
* Support for file additions, modifications, and deletions
* Bandwidth-efficient incremental updates
* Progress tracking for each file
* Configurable file filtering

Configuration Options
---------------------

* ``SetApplicationDirectory()`` - Set base directory path
* ``AddUploadsFromSubdirectory()`` - Add directory to serve
* ``DownloadFromSubdirectory()`` - Request directory sync
* ``ClearUploads()`` - Remove all upload directories
* ``GenerateHashes()`` - Pre-compute file hashes

See Also
--------

* :doc:`file-list-transfer` - Underlying file transfer
* :doc:`autopatcher` - Full patching solution
* :doc:`../utilities/string-compressor` - Data compression
