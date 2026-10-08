/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_DirectoryDeltaTransfer==1 && _RAKNET_SUPPORT_FileOperations==1

#include "mafianet/directory_delta_transfer.h"
#include "mafianet/file_list.h"
#include "mafianet/string_compressor.h"
#include "mafianet/peer_interface.h"
#include "mafianet/file_list_transfer.h"
#include "mafianet/file_list_transfer_cb_interface.h"
#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/file_operations.h"
#include "mafianet/ds_list.h"
#include "mafianet/super_fast_hash.h"
#include "mafianet/incremental_read_interface.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

using namespace MafiaNet;

// Writes each received file below outputSubdir. A file pushed through an IncrementalReadInterface
// is written chunk by chunk as it arrives, at its offset, instead of being held in memory whole until
// its last chunk: a download then costs one chunk of memory however large its files are. A file sent
// in one piece is written when it completes, as before.
class DDTCallback : public FileListTransferCBInterface
{
public:
	unsigned subdirLen;
	char outputSubdir[512];
	FileListTransferCBInterface *onFileCallback;

	DDTCallback() {}
	virtual ~DDTCallback() {}

	virtual bool OnFile(OnFileStruct *onFileStruct)
	{
		char fullPathToDir[1024];

		// A streamed file is already on disk, written by OnFileProgress.
		const bool streamed = streamedFiles.GetIndexOf(onFileStruct->fileIndex) != MAX_UNSIGNED_LONG;
		if (streamed)
			streamedFiles.RemoveAtIndexFast(streamedFiles.GetIndexOf(onFileStruct->fileIndex));
		else if (onFileStruct->fileData && TargetPath(onFileStruct->fileName, fullPathToDir))
			WriteFileWithDirectories(fullPathToDir, (char*)onFileStruct->fileData, (unsigned int ) onFileStruct->byteLengthOfThisFile);

		return onFileCallback->OnFile(onFileStruct);
	}

	virtual void OnFileProgress(FileProgressStruct *fps)
	{
		char fullPathToDir[1024];

		// iriDataChunk is set only once a whole chunk has arrived; partial notifications carry none.
		if (fps->iriDataChunk && fps->dataChunkLength > 0 && TargetPath(fps->onFileStruct->fileName, fullPathToDir))
		{
			if (WriteChunk(fullPathToDir, fps->iriWriteOffset, fps->iriDataChunk, fps->dataChunkLength))
			{
				if (streamedFiles.GetIndexOf(fps->onFileStruct->fileIndex) == MAX_UNSIGNED_LONG)
					streamedFiles.Insert(fps->onFileStruct->fileIndex, _FILE_AND_LINE_);
				// Nothing for FileListTransfer to keep: the chunk is on disk.
				fps->allocateIrIDataChunkAutomatically = false;
			}
		}

		onFileCallback->OnFileProgress(fps);
	}
	virtual bool OnDownloadComplete(DownloadCompleteStruct *dcs)
	{
		return onFileCallback->OnDownloadComplete(dcs);
	}

private:
	bool TargetPath(const char *fileName, char *out)
	{
		if (subdirLen >= strlen(fileName))
		{
			out[0]=0;
			return false;
		}
		strcpy_s(out, 1024, outputSubdir);
		strcat_s(out, 1024, fileName+subdirLen);
		return true;
	}

	// The first chunk creates the file (and its directories); later ones are written in place.
	static bool WriteChunk(const char *path, unsigned int offset, const char *data, unsigned int length)
	{
		if (offset == 0)
			return WriteFileWithDirectories(path, (char*) data, length);

		FILE *fp;
		if (fopen_s(&fp, path, "r+b") != 0 || fp == 0)
			return false;
#ifdef _WIN32
		const bool positioned = _fseeki64(fp, (__int64) offset, SEEK_SET) == 0;
#else
		const bool positioned = fseeko(fp, (off_t) offset, SEEK_SET) == 0;
#endif
		const bool written = positioned && fwrite(data, 1, length, fp) == length;
		fclose(fp);
		return written;
	}

	// File indices of this set being written chunk by chunk.
	DataStructures::List<unsigned int> streamedFiles;
};

STATIC_FACTORY_DEFINITIONS(DirectoryDeltaTransfer,DirectoryDeltaTransfer);

DirectoryDeltaTransfer::DirectoryDeltaTransfer()
{
	applicationDirectory[0]=0;
	fileListTransfer=0;
	availableUploads = MafiaNet::OP_NEW<FileList>( _FILE_AND_LINE_ );
	priority=MafiaNet::Priority::High;
	orderingChannel=0;
	incrementalReadInterface=0;
}
DirectoryDeltaTransfer::~DirectoryDeltaTransfer()
{
	MafiaNet::OP_DELETE(availableUploads, _FILE_AND_LINE_);
}
void DirectoryDeltaTransfer::SetFileListTransferPlugin(FileListTransfer *flt)
{
	if (fileListTransfer)
	{
		DataStructures::List<FileListProgress*> fileListProgressList;
		fileListTransfer->GetCallbacks(fileListProgressList);
		unsigned int i;
		for (i=0; i < fileListProgressList.Size(); i++)
			availableUploads->RemoveCallback(fileListProgressList[i]);
	}

	fileListTransfer=flt;

	if (flt)
	{
		DataStructures::List<FileListProgress*> fileListProgressList;
		flt->GetCallbacks(fileListProgressList);
		unsigned int i;
		for (i=0; i < fileListProgressList.Size(); i++)
			availableUploads->AddCallback(fileListProgressList[i]);
	}
	else
	{
		availableUploads->ClearCallbacks();
	}
}
void DirectoryDeltaTransfer::SetApplicationDirectory(const char *pathToApplication)
{
	if (pathToApplication==0 || pathToApplication[0]==0)
		applicationDirectory[0]=0;
	else
	{
		strncpy_s(applicationDirectory, pathToApplication, 510);
		if (applicationDirectory[strlen(applicationDirectory)-1]!='/' && applicationDirectory[strlen(applicationDirectory)-1]!='\\')
			strcat_s(applicationDirectory, "/");
		applicationDirectory[511]=0;
	}
}
void DirectoryDeltaTransfer::SetUploadSendParameters(MafiaNet::Priority _priority, char _orderingChannel)
{
	priority=_priority;
	orderingChannel=_orderingChannel;
}
void DirectoryDeltaTransfer::AddFile(const char* filePath, const char* fileName)
{
	// Recorded the way AddUploadsFromSubdirectory records a file -- its hash and length, not its
	// contents -- because that is what a downloader sends to compare against. Holding the contents
	// made every comparison fail, so every download re-sent every file, and kept each upload in
	// memory whole. A download reads the file from disk when it is sent.
	FILE *fp;
	if (filePath == 0 || fileName == 0 || fopen_s(&fp, filePath, "rb") != 0 || fp == 0)
		return;
	fseek(fp, 0, SEEK_END);
	const long length = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	unsigned int hash = SuperFastHashFilePtr(fp);
	fclose(fp);
	if (length < 0)
		return;
	if (MafiaNet::BitStream::DoEndianSwap())
		MafiaNet::BitStream::ReverseBytesInPlace((unsigned char*) &hash, sizeof(hash));
	availableUploads->AddFile(fileName, filePath, (const char*) &hash, (unsigned int) sizeof(hash), (unsigned int) length, FileListNodeContext(0, 0, 0, 0));
}
void DirectoryDeltaTransfer::AddUploadsFromSubdirectory(const char *subdir)
{
	availableUploads->AddFilesFromDirectory(applicationDirectory, subdir, true, false, true, FileListNodeContext(0,0,0,0));
}
unsigned short DirectoryDeltaTransfer::DownloadFromSubdirectory(FileList &localFiles, const char *subdir, const char *outputSubdir, bool prependAppDirToOutputSubdir, SystemAddress host, FileListTransferCBInterface *onFileCallback, MafiaNet::Priority _priority, char _orderingChannel, FileListProgress *cb)
{
	RakAssert(host!=UNASSIGNED_SYSTEM_ADDRESS);

	DDTCallback *transferCallback;

	localFiles.AddCallback(cb);

	// Prepare the callback data
	transferCallback = MafiaNet::OP_NEW<DDTCallback>( _FILE_AND_LINE_ );
	if (subdir && subdir[0])
	{
		transferCallback->subdirLen=(unsigned int)strlen(subdir);
		if (subdir[transferCallback->subdirLen-1]!='/' && subdir[transferCallback->subdirLen-1]!='\\')
			transferCallback->subdirLen++;
	}
	else
		transferCallback->subdirLen=0;
	if (prependAppDirToOutputSubdir)
		strcpy_s(transferCallback->outputSubdir, applicationDirectory);
	else
		transferCallback->outputSubdir[0]=0;
	if (outputSubdir)
		strcat_s(transferCallback->outputSubdir, outputSubdir);
	if (transferCallback->outputSubdir[strlen(transferCallback->outputSubdir)-1]!='/' && transferCallback->outputSubdir[strlen(transferCallback->outputSubdir)-1]!='\\')
		strcat_s(transferCallback->outputSubdir, "/");
	transferCallback->onFileCallback=onFileCallback;

	// Setup the transfer plugin to get the response to this download request
	unsigned short setId = fileListTransfer->SetupReceive(transferCallback, true, host);

	// Send to the host, telling it to process this request
	MafiaNet::BitStream outBitstream;
	outBitstream.Write((MessageID)ID_DDT_DOWNLOAD_REQUEST);
	outBitstream.Write(setId);
	StringCompressor::Instance()->EncodeString(subdir, 256, &outBitstream);
	StringCompressor::Instance()->EncodeString(outputSubdir, 256, &outBitstream);
	localFiles.Serialize(&outBitstream);
	SendUnified(&outBitstream, _priority, MafiaNet::Reliability::ReliableOrdered, _orderingChannel, host, false);

	return setId;
}
unsigned short DirectoryDeltaTransfer::DownloadFromSubdirectory(const char *subdir, const char *outputSubdir, bool prependAppDirToOutputSubdir, SystemAddress host, FileListTransferCBInterface *onFileCallback, MafiaNet::Priority _priority, char _orderingChannel, FileListProgress *cb)
{
	FileList localFiles;
	// Get a hash of all the files that we already have (if any)
	localFiles.AddFilesFromDirectory(prependAppDirToOutputSubdir ? applicationDirectory : 0, outputSubdir, true, false, true, FileListNodeContext(0,0,0,0));
	return DownloadFromSubdirectory(localFiles, subdir, outputSubdir, prependAppDirToOutputSubdir, host, onFileCallback, _priority, _orderingChannel, cb);
}
void DirectoryDeltaTransfer::GenerateHashes(FileList &localFiles, const char *outputSubdir, bool prependAppDirToOutputSubdir)
{
	localFiles.AddFilesFromDirectory(prependAppDirToOutputSubdir ? applicationDirectory : 0, outputSubdir, true, false, true, FileListNodeContext(0,0,0,0));
}
void DirectoryDeltaTransfer::ClearUploads(void)
{
	availableUploads->Clear();
}
void DirectoryDeltaTransfer::OnDownloadRequest(Packet *packet)
{
	char subdir[256];
	char remoteSubdir[256];
	MafiaNet::BitStream inBitstream(packet->data, packet->length, false);
	FileList remoteFileHash;
	FileList delta;
	unsigned short setId;
    inBitstream.IgnoreBits(8);
	inBitstream.Read(setId);
	StringCompressor::Instance()->DecodeString(subdir, 256, &inBitstream);
	StringCompressor::Instance()->DecodeString(remoteSubdir, 256, &inBitstream);
	if (remoteFileHash.Deserialize(&inBitstream)==false)
	{
#ifdef _DEBUG
		RakAssert(0);
#endif
		return;
	}

	availableUploads->GetDeltaToCurrent(&remoteFileHash, &delta, subdir, remoteSubdir);
	if (incrementalReadInterface==0)
		delta.PopulateDataFromDisk(applicationDirectory, true, false, true);
	else
		delta.FlagFilesAsReferences();

	// This will call the ddtCallback interface that was passed to FileListTransfer::SetupReceive on the remote system
	fileListTransfer->Send(&delta, rakPeerInterface, packet->systemAddress, setId, priority, orderingChannel, incrementalReadInterface, chunkSize);
}
PluginReceiveResult DirectoryDeltaTransfer::OnReceive(Packet *packet)
{
	switch (packet->data[0]) 
	{
	case ID_DDT_DOWNLOAD_REQUEST:
		OnDownloadRequest(packet);
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	}

	return RR_CONTINUE_PROCESSING;
}

unsigned DirectoryDeltaTransfer::GetNumberOfFilesForUpload(void) const
{
	return availableUploads->fileList.Size();
}

void DirectoryDeltaTransfer::SetDownloadRequestIncrementalReadInterface(IncrementalReadInterface *_incrementalReadInterface, unsigned int _chunkSize)
{
	incrementalReadInterface=_incrementalReadInterface;
	chunkSize=_chunkSize;
}

#endif // _RAKNET_SUPPORT_*
