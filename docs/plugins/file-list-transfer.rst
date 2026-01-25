FileListTransfer Plugin
=======================

The FileListTransfer plugin enables efficient transfer of multiple files between
systems. It supports compression, incremental transfers, and progress callbacks,
making it ideal for game asset distribution and patching.

Basic Usage
-----------

**Sending files:**

.. code-block:: cpp

   #include "slikenet/FileListTransfer.h"
   #include "slikenet/FileList.h"

   MafiaNet::FileListTransfer flt;
   peer->AttachPlugin(&flt);

   // Create file list
   MafiaNet::FileList files;
   files.AddFile("assets/texture.png", "texture.png",
                 FileListNodeContext(0, 0, 0, 0));
   files.AddFile("assets/model.obj", "model.obj",
                 FileListNodeContext(0, 0, 0, 0));

   // Send to remote system
   unsigned short setId = flt.SetupReceive(&transferCallback, false, targetAddress);
   flt.Send(&files, peer, targetAddress, setId, HIGH_PRIORITY, 0, false);

**Receiving files:**

.. code-block:: cpp

   class MyTransferCallback : public MafiaNet::FileListTransferCBInterface {
   public:
       bool OnFile(OnFileStruct* ofs) override {
           // Save received file
           FILE* fp = fopen(ofs->fileName, "wb");
           fwrite(ofs->fileData, 1, ofs->byteLengthOfThisFile, fp);
           fclose(fp);
           return true;  // Continue receiving
       }

       void OnFileProgress(FileProgressStruct* fps) override {
           printf("Progress: %d/%d bytes\n",
                  fps->partCount * fps->partLength,
                  fps->byteLengthOfThisFile);
       }
   };

   MyTransferCallback callback;
   flt.SetupReceive(&callback, false, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);

Key Features
------------

* Transfer multiple files in a single operation
* Built-in compression support (requires zlib)
* Incremental transfer with delta encoding
* Progress callbacks for UI updates
* Automatic chunking for large files
* Resume capability after disconnection
* Memory-efficient streaming mode

Configuration Options
---------------------

* ``SetupReceive()`` - Configure receive callback
* ``Send()`` - Initiate file transfer
* ``CancelReceive()`` - Abort ongoing transfer
* ``GetPendingFilesToAddress()`` - Check transfer queue
* ``SetCallbackThreadSafe()`` - Enable thread-safe callbacks

See Also
--------

* :doc:`directory-delta-transfer` - Directory synchronization
* :doc:`autopatcher` - Delta patching system
* :doc:`../basics/bitstreams` - Binary data handling
