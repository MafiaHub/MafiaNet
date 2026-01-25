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

#include "mafianet/GetTime.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/statistics.h"
#include "mafianet/DirectoryDeltaTransfer.h"
#include "mafianet/FileListTransfer.h"
#include <cstdio>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/Kbhit.h"
#include "mafianet/FileList.h"
#include "mafianet/DataCompressor.h"
#include "mafianet/FileListTransferCBInterface.h"
#include "mafianet/sleep.h"
#include "mafianet/IncrementalReadInterface.h"
#include "mafianet/PacketizedTCP.h"
#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#ifdef _WIN32
#include "mafianet/WindowsIncludes.h" // Sleep
#else
#include <unistd.h> // usleep
#endif

#define USE_TCP

class TestCB : public MafiaNet::FileListTransferCBInterface
{
public:
	bool OnFile(
		OnFileStruct *onFileStruct)
	{
		assert(onFileStruct->byteLengthOfThisFile >= onFileStruct->bytesDownloadedForThisFile);
        printf("%i. (100%%) %i/%i %s %ib / %ib\n", onFileStruct->setID, onFileStruct->fileIndex+1, onFileStruct->numberOfFilesInThisSet, onFileStruct->fileName, onFileStruct->byteLengthOfThisFile, onFileStruct->byteLengthOfThisSet);

		// Return true to have RakNet delete the memory allocated to hold this file.
		// False if you hold onto the memory, and plan to delete it yourself later
		return true;
	}

	virtual void OnFileProgress(FileProgressStruct *fps)
	{
		assert(fps->onFileStruct->byteLengthOfThisFile >= fps->onFileStruct->bytesDownloadedForThisFile);
		printf("%i (%i%%) %i/%i %s %ib / %ib\n", fps->onFileStruct->setID, (int) (100.0*(double)fps->partCount/(double)fps->partTotal),
			fps->onFileStruct->fileIndex+1,
			fps->onFileStruct->numberOfFilesInThisSet,
			fps->onFileStruct->fileName,
			fps->onFileStruct->byteLengthOfThisFile,
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


int main(void)
{
	int ch;

#ifdef USE_TCP
	MafiaNet::PacketizedTCP tcp1;
#else
	MafiaNet::RakPeerInterface *rakPeer;
#endif

	// directoryDeltaTransfer is the main plugin that does the work for this sample.
	MafiaNet::DirectoryDeltaTransfer directoryDeltaTransfer;
	// The fileListTransfer plugin is used by the DirectoryDeltaTransfer plugin and must also be registered (you could use this yourself too if you wanted, of course).
	MafiaNet::FileListTransfer fileListTransfer;
	// Read files in parts, rather than the whole file from disk at once
	MafiaNet::IncrementalReadInterface iri;
	directoryDeltaTransfer.SetDownloadRequestIncrementalReadInterface(&iri, 1000000);

#ifdef USE_TCP
	tcp1.AttachPlugin(&directoryDeltaTransfer);
	tcp1.AttachPlugin(&fileListTransfer);
#else
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	rakPeer->AttachPlugin(&directoryDeltaTransfer);
	rakPeer->AttachPlugin(&fileListTransfer);
	// Get download progress notifications.  Handled by the plugin.
	rakPeer->SetSplitMessageProgressInterval(100);
#endif
	directoryDeltaTransfer.SetFileListTransferPlugin(&fileListTransfer);

	printf("This sample demonstrates the plugin to incrementally transfer compressed\n");
	printf("deltas of directories.  In essence, it's a simple autopatcher.\n");
	printf("Unlike the full autopatcher, it has no dependencies.  It is suitable for\n");
	printf("patching from non-dedicated servers at runtime.\n");
	printf("Difficulty: Intermediate\n\n");

	printf("Enter listen port. Enter for default. If running two instances on the\nsame computer, use 0 for the client.\n");
	unsigned short localPort;
	char str[256];
	Gets(str, sizeof(str));
	if (str[0]==0)
		localPort=60000;
	else {
		const int intLocalPort = atoi(str);
		if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
			return 2;
		}
		localPort = static_cast<unsigned short>(intLocalPort);
	}
	MafiaNet::SocketDescriptor socketDescriptor(localPort,0);
#ifdef USE_TCP
	SLNET_VERIFY(tcp1.Start(localPort, 8));
#else
	if (rakPeer->Startup(8,&socketDescriptor, 1)!= MafiaNet::RAKNET_STARTED)
	{
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
		printf("RakNet initialize failed.  Possibly duplicate port.\n");
		return 1;
	}
	rakPeer->SetMaximumIncomingConnections(8);
#endif

	printf("Commands:\n");
	printf("(S)et application directory.\n");
	printf("(A)dd allowed uploads from subdirectory.\n");
	printf("(D)ownload from subdirectory.\n");
	printf("(C)lear allowed uploads.\n");
	printf("C(o)nnect to another system.\n");
	printf("(Q)uit.\n");

	MafiaNet::SystemAddress sysAddrZero= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	// MafiaNet::TimeMS nextStatTime = MafiaNet::GetTimeMS() + 1000;

	MafiaNet::Packet *p;
	for(;;)
	{
		/*
		if (//directoryDeltaTransfer.GetNumberOfFilesForUpload()>0 &&
			MafiaNet::GetTimeMS() > nextStatTime)
		{
			// If sending, periodically show connection stats
			char statData[2048];
			RakNetStatistics *statistics = rakPeer->GetStatistics(rakPeer->GetSystemAddressFromIndex(0));
		//	if (statistics->messagesOnResendQueue>0 || statistics->internalOutputQueueSize>0)
			if (rakPeer->GetSystemAddressFromIndex(0)!=MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			{
				StatisticsToString(statistics, statData, 2048, 2);
				printf("%s\n", statData);
			}
			
			nextStatTime=MafiaNet::GetTimeMS()+5000;
		}
		*/

		// Process packets
#ifdef USE_TCP
		p=tcp1.Receive();
#else
		p=rakPeer->Receive();
#endif

#ifdef USE_TCP
		MafiaNet::SystemAddress sa;
		sa=tcp1.HasNewIncomingConnection();
		if (sa!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			printf("ID_NEW_INCOMING_CONNECTION\n");
			sysAddrZero=sa;
		}
		if (tcp1.HasLostConnection()!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_DISCONNECTION_NOTIFICATION\n");
		if (tcp1.HasFailedConnectionAttempt()!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_CONNECTION_ATTEMPT_FAILED\n");
		sa=tcp1.HasCompletedConnectionAttempt();
		if (sa!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
			sysAddrZero=sa;
		}
#endif

		while (p)
		{

#ifdef USE_TCP
			tcp1.DeallocatePacket(p);
			tcp1.Receive();
#else

			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
			{
				printf("ID_NEW_INCOMING_CONNECTION\n");
				sysAddrZero=p->systemAddress;
			}
			else if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
				sysAddrZero=p->systemAddress;
			}
			else if (p->data[0]==ID_DISCONNECTION_NOTIFICATION)
				printf("ID_DISCONNECTION_NOTIFICATION\n");
			else if (p->data[0]==ID_CONNECTION_LOST)
				printf("ID_CONNECTION_LOST\n");
			else if (p->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
				printf("ID_CONNECTION_ATTEMPT_FAILED\n");
			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
#endif
		}

		if (_kbhit())
		{
			ch=_getch();
			if (ch=='s')
			{
				printf("Enter application directory\n");
				Gets(str, sizeof(str));
				if (str[0]==0)
					strcpy_s(str, "C:/Temp");
				directoryDeltaTransfer.SetApplicationDirectory(str);
				printf("This directory will be prefixed to upload and download subdirectories.\n");
			}
			else if (ch=='a')
			{
				printf("Enter uploads subdirectory\n");
				Gets(str, sizeof(str));
				directoryDeltaTransfer.AddUploadsFromSubdirectory(str);
				printf("%i files for upload.\n", directoryDeltaTransfer.GetNumberOfFilesForUpload());
			}
			else if (ch=='d')
			{
				char subdir[256];
				char outputSubdir[256];
				printf("Enter remote subdirectory to download from.\n");
				printf("This directory may be any uploaded directory, or a subdir therein.\n");
				Gets(subdir,sizeof(subdir));
				printf("Enter subdirectory to output to.\n");
				Gets(outputSubdir,sizeof(outputSubdir));
                
				unsigned short setId;

				setId=directoryDeltaTransfer.DownloadFromSubdirectory(subdir, outputSubdir, true, sysAddrZero, &transferCallback, HIGH_PRIORITY, 0, 0);
				if (setId==(unsigned short)-1)
					printf("Download failed.  Host unreachable.\n");
				else
					printf("Downloading set %i\n", setId);
			}
			else if (ch=='c')
			{
				directoryDeltaTransfer.ClearUploads();
				printf("Uploads cleared.\n");
			}
			else if (ch=='o')
			{
				char host[256];
				printf("Enter host IP: ");
				Gets(host,sizeof(host));
				if (host[0]==0)
					strcpy_s(host, "127.0.0.1");
				unsigned short remotePort;
				printf("Enter host port: ");
				Gets(str, sizeof(str));
				if (str[0]==0)
					remotePort=60000;
				else {
					const int intRemotePort = atoi(str);
					if ((intRemotePort < 0) || (intRemotePort > std::numeric_limits<unsigned short>::max())) {
						printf("Specified remote port %d is outside valid bounds [0, %u]", intRemotePort, std::numeric_limits<unsigned short>::max());
						return 3;
					}
					remotePort = static_cast<unsigned short>(intRemotePort);
				}
#ifdef USE_TCP
				tcp1.Connect(host,remotePort,false);
#else
				rakPeer->Connect(host, remotePort, 0, 0);
#endif
				printf("Connecting.\n");
			}
			else if (ch=='q')
			{
				printf("Bye!\n");
#ifdef USE_TCP
				tcp1.Stop();
#else
				rakPeer->Shutdown(1000,0);
#endif
				break;
			}
		}

		// Keeps the threads responsive
		RakSleep(0);
	}

#ifdef USE_TCP
#else
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
#endif

	return 0;
}
