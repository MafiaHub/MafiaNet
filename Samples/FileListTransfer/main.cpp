/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "slikenet/peerinterface.h"
#include "slikenet/FileListTransfer.h"
#include "slikenet/sleep.h"

#include "slikenet/MessageIdentifiers.h"
#include "slikenet/FileListTransferCBInterface.h"
#include "slikenet/FileOperations.h"
#include "slikenet/SuperFastHash.h"
#include "slikenet/assert.h"
#include "slikenet/BitStream.h"
#include "slikenet/IncrementalReadInterface.h"
#include "slikenet/PacketizedTCP.h"
#include "slikenet/SocketLayer.h"
#include <stdio.h>
#include "slikenet/Gets.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

MafiaNet::RakString file;
MafiaNet::RakString fileCopy;

//const char *file = "c:/temp/unittest.vcproj";
//const char *fileCopy = "c:/temp/unittest_copy.vcproj";

#define USE_TCP

class TestCB : public MafiaNet::FileListTransferCBInterface
{
public:
	bool OnFile(
		OnFileStruct *onFileStruct)
	{
		printf("OnFile: %u. (100%%) %u/%u %s %ub / %ub\n",
			onFileStruct->setID,
			onFileStruct->fileIndex+1,
			onFileStruct->numberOfFilesInThisSet,
			onFileStruct->fileName,
			onFileStruct->byteLengthOfThisFile,
			onFileStruct->byteLengthOfThisSet);


		FILE *fp;
		fopen_s(&fp, fileCopy.C_String(), "wb");
		fwrite(onFileStruct->fileData, onFileStruct->byteLengthOfThisFile, 1, fp);
		fclose(fp);

		// Make sure it worked
		unsigned int hash1 = SuperFastHashFile(file.C_String());
		if (MafiaNet::BitStream::DoEndianSwap())
			MafiaNet::BitStream::ReverseBytesInPlace((unsigned char*) &hash1, sizeof(hash1));
		unsigned int hash2 = SuperFastHashFile(fileCopy.C_String());
		if (MafiaNet::BitStream::DoEndianSwap())
			MafiaNet::BitStream::ReverseBytesInPlace((unsigned char*) &hash2, sizeof(hash2));
		RakAssert(hash1==hash2);

		// Return true to have RakNet delete the memory allocated to hold this file.
		// False if you hold onto the memory, and plan to delete it yourself later
		return true;
	}

	virtual void OnFileProgress(FileProgressStruct *fps)
	{
		printf("OnFileProgress: %u partCount=%u partTotal=%u (%u%%) %u/%u %s %ub/%ub %ub/%ub total\n",
			fps->onFileStruct->setID,
			fps->partCount, fps->partTotal, (unsigned int) (100.0*(double)fps->onFileStruct->bytesDownloadedForThisFile/(double)fps->onFileStruct->byteLengthOfThisFile),
			fps->onFileStruct->fileIndex+1,
			fps->onFileStruct->numberOfFilesInThisSet,
			fps->onFileStruct->fileName,
			fps->onFileStruct->bytesDownloadedForThisFile,
			fps->onFileStruct->byteLengthOfThisFile,
			fps->onFileStruct->bytesDownloadedForThisSet,
			fps->onFileStruct->byteLengthOfThisSet);
	}

	virtual bool OnDownloadComplete(DownloadCompleteStruct *dcs)
	{
		// unused parameters
		(void)dcs;

		printf("Download complete.\n");

		// Returning false automatically deallocates the automatically allocated handler that was created by DirectoryDeltaTransfer
		return false;
	}

} transferCallback;

// Sender progress notification
class TestFileListProgress : public MafiaNet::FileListProgress
{
    virtual void OnFilePush(const char *fileName, unsigned int fileLengthBytes, unsigned int offset, unsigned int bytesBeingSent, bool done, MafiaNet::SystemAddress targetSystem, unsigned short setID)
	{
		// unused parameters
		(void)fileLengthBytes;
		(void)done;
		(void)targetSystem;
		(void)setID;

		printf("Sending %s bytes=%u offset=%u\n", fileName, bytesBeingSent, offset);
	}

    virtual void OnFilePushesComplete(MafiaNet::SystemAddress systemAddress, unsigned short setID )
	{
		// unused parameters
		(void)setID;

		char str[32];
		systemAddress.ToString(true, (char*) str, static_cast<size_t>(32));
		RAKNET_DEBUG_PRINTF("File pushes complete to %s\n", str);	
	}
	virtual void OnSendAborted(MafiaNet::SystemAddress systemAddress )
	{
		char str[32];
		systemAddress.ToString(true, (char*) str, static_cast<size_t>(32));
		RAKNET_DEBUG_PRINTF("Send aborted to %s\n", str);	
	}

} testFileListProgress;

int main()
{
	printf("This sample demonstrates incrementally sending a file with\n");
	printf("the FileListTransferPlugin. Incremental sends will read and send the.\n");
	printf("file only as needed, rather than putting the whole file in memory.\n");
	printf("This is to support sending large files to many users at the same time.\n");
	printf("Difficulty: Intermediate\n\n");

	TestCB testCB;
	MafiaNet::FileListTransfer flt1, flt2;
#ifdef USE_TCP
	MafiaNet::PacketizedTCP tcp1, tcp2;
	#if RAKNET_SUPPORT_IPV6==1
	const bool testInet6=true;
	#else
	const bool testInet6=false;
	#endif
	if (testInet6)
	{
		tcp1.Start(60000,1,1,-99999,AF_INET6);
		tcp2.Start(60001,1,1,-99999,AF_INET6);
		tcp2.Connect("::1",60000,false,AF_INET6);
	}
	else
	{
		tcp1.Start(60000,1,1,-99999,AF_INET);
		tcp2.Start(60001,1,1,-99999,AF_INET);
		tcp2.Connect("127.0.0.1",60000,false,AF_INET);
	}
	tcp1.AttachPlugin(&flt1);
	tcp2.AttachPlugin(&flt2);
#else
	MafiaNet::RakPeerInterface *peer1 = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::RakPeerInterface *peer2 = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::SocketDescriptor sd1(60000,0),sd2(60001,0);
	peer1->Startup(1,&sd1,1);
	peer2->Startup(1,&sd2,1);
	peer1->SetMaximumIncomingConnections(1);
	peer2->Connect("127.0.0.1",60000,0,0,0);
	peer1->AttachPlugin(&flt1);
	peer2->AttachPlugin(&flt2);
	peer1->SetSplitMessageProgressInterval(9);
	peer2->SetSplitMessageProgressInterval(9);
#endif
	// Print sending progress notifications
	flt1.AddCallback(&testFileListProgress);
	// Run incremental reads in a thread so the read does not block the main thread
	flt1.StartIncrementalReadThreads(1);
	MafiaNet::FileList fileList;
	MafiaNet::IncrementalReadInterface incrementalReadInterface;
	printf("Enter complete filename with path to test:\n");
	char str[256];
	Gets(str, sizeof(str));
	if (str[0]==0)
		strcpy_s(str, "D:\\RakNet\\Lib\\RakNetLibStaticDebug.lib");
	file=str;
	fileCopy=file+"_copy";
	// Reference this file, rather than add it in memory. Will send 1000 byte chunks. The reason to do this is so the whole file does not have to be in memory at once
	unsigned int fileLength = GetFileLength(file.C_String());
	if (fileLength==0)
	{
		printf("Test file %s not found.\n", file.C_String());

#ifdef USE_TCP
#else
		MafiaNet::RakPeerInterface::DestroyInstance(peer1);
		MafiaNet::RakPeerInterface::DestroyInstance(peer2);
#endif
		return 1;
	}
	fileList.AddFile(file.C_String(), file.C_String(), 0, fileLength, fileLength, FileListNodeContext(0,0,0,0), true);
	// Wait for the connection
	printf("File added.\n");
	RakSleep(100);
	MafiaNet::Packet *packet1, *packet2;
	for(;;)
	{
#ifdef USE_TCP
		MafiaNet::SystemAddress sa;
		sa=tcp2.HasCompletedConnectionAttempt();
		if (sa!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			flt2.SetupReceive(&testCB, false, sa);
			break;
		}
#else
		// Wait for the connection request to be accepted
		packet2=peer2->Receive();
		if (packet2 && packet2->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
		{
			flt2.SetupReceive(&testCB, false, packet2->systemAddress);
			peer2->DeallocatePacket(packet2);
			break;
		}
		peer2->DeallocatePacket(packet2);
#endif
		RakSleep(30);
	}

	// When connected, send the file. Since the file is a reference, it will be sent incrementally
	for(;;)
	{
#ifdef USE_TCP
		packet1=tcp1.Receive();
		packet2=tcp2.Receive();
		MafiaNet::SystemAddress sa;
		sa = tcp1.HasNewIncomingConnection();
		if (sa!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			flt1.Send(&fileList,0,sa,0,HIGH_PRIORITY,0, &incrementalReadInterface, 2000 * 1024);
		tcp1.DeallocatePacket(packet1);
		tcp2.DeallocatePacket(packet2);
#else
		packet1=peer1->Receive();
		packet2=peer2->Receive();
		if (packet1 && packet1->data[0]==ID_NEW_INCOMING_CONNECTION)
			flt1.Send(&fileList,peer1,packet1->systemAddress,0,HIGH_PRIORITY,0, &incrementalReadInterface, 2000000);
		
		peer1->DeallocatePacket(packet1);
		peer2->DeallocatePacket(packet2);

#endif
		RakSleep(0);
	}
	
#ifdef USE_TCP
#else
	MafiaNet::RakPeerInterface::DestroyInstance(peer1);
	MafiaNet::RakPeerInterface::DestroyInstance(peer1);
#endif


	return 0;
}
