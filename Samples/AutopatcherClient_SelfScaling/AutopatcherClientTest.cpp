/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

// serverIP should be ip address of patcher
// pathToGame should be something like "C:\Games\mygame", whatever the installation path was
// gameName should be patcherHostSubdomainURL found in AutopatcherServer_SelfScaling
// patchImmediately should be 1
// portToStartOn should be 0
// serverPort should be 60000, see LISTEN_PORT_TCP_PATCHER in AutopatcherServer_SelfScaling
// fullScan should be '1' to fully scan all files. 0 to use the last patch.

// Common includes
#include <stdio.h>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/Kbhit.h"

#include "mafianet/GetTime.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/StringCompressor.h"
#include "mafianet/PacketizedTCP.h"
#include "mafianet/socket2.h"

// Client only includes
#include "mafianet/FileListTransferCBInterface.h"
#include "mafianet/FileListTransfer.h"
#include "AutopatcherClient.h"
#include "mafianet/AutopatcherPatchContext.h"
#include "mafianet/Gets.h"
#include "mafianet/sleep.h"
#include "mafianet/CloudClient.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

void GetServers(MafiaNet::CloudClient *cloudClient, MafiaNet::RakNetGUID serverGuid);
void GetClientSubscription(MafiaNet::CloudClient *cloudClient, MafiaNet::RakNetGUID serverGuid);
void UploadInstanceToCloud(MafiaNet::CloudClient *cloudClient, MafiaNet::RakNetGUID serverGuid);

#define CLOUD_CLIENT_PRIMARY_KEY "SelfScaling_Patcher_PK"

class TestCB : public MafiaNet::AutopatcherClientCBInterface
{
public:
	virtual bool OnFile(OnFileStruct *onFileStruct)
	{
		if (onFileStruct->context.op==PC_HASH_1_WITH_PATCH || onFileStruct->context.op==PC_HASH_2_WITH_PATCH)
			printf("Patched: ");
		else if (onFileStruct->context.op==PC_WRITE_FILE)
			printf("Written: ");
		else if (onFileStruct->context.op==PC_ERROR_FILE_WRITE_FAILURE)
			printf("Write Failure: ");
		else if (onFileStruct->context.op==PC_ERROR_PATCH_TARGET_MISSING)
			printf("Patch target missing: ");
		else if (onFileStruct->context.op==PC_ERROR_PATCH_APPLICATION_FAILURE)
			printf("Patch process failure: ");
		else if (onFileStruct->context.op==PC_ERROR_PATCH_RESULT_CHECKSUM_FAILURE)
			printf("Patch checksum failure: ");
		else if (onFileStruct->context.op==PC_NOTICE_WILL_COPY_ON_RESTART)
			printf("Copy pending restart: ");
		else if (onFileStruct->context.op==PC_NOTICE_FILE_DOWNLOADED)
			printf("Downloaded: ");
		else if (onFileStruct->context.op==PC_NOTICE_FILE_DOWNLOADED_PATCH)
			printf("Downloaded Patch: ");
		else
			RakAssert(0);


		printf("%i. (100%%) %i/%i %s %ib / %ib\n", onFileStruct->setID, onFileStruct->fileIndex+1, onFileStruct->numberOfFilesInThisSet,
			onFileStruct->fileName, onFileStruct->byteLengthOfThisFile,
			onFileStruct->byteLengthOfThisSet);

		// Return false for the file data to be deallocated automatically
		return false;
	}

	virtual void OnFileProgress(FileProgressStruct *fps)
	{
		printf("Downloading: %i. (%i%%) %i/%i %s %ib/%ib %ib/%ib total\n", fps->onFileStruct->setID,
			(int) (100.0*(double)fps->onFileStruct->bytesDownloadedForThisFile/(double)fps->onFileStruct->byteLengthOfThisFile),
			fps->onFileStruct->fileIndex+1, fps->onFileStruct->numberOfFilesInThisSet, fps->onFileStruct->fileName,
			fps->onFileStruct->bytesDownloadedForThisFile,
			fps->onFileStruct->byteLengthOfThisFile,			
			fps->onFileStruct->bytesDownloadedForThisSet,
			fps->onFileStruct->byteLengthOfThisSet
			);
	}

	virtual PatchContext ApplyPatchBase(const char *oldFilePath, char **newFileContents, unsigned int *newFileSize, char *patchContents, unsigned int patchSize, uint32_t patchAlgorithm)
	{
		if (patchAlgorithm==0)
		{
			return ApplyPatchBSDiff(oldFilePath, newFileContents, newFileSize, patchContents, patchSize);
		}
		else
		{
			char WORKING_DIRECTORY[MAX_PATH];
			GetTempPathA(MAX_PATH, WORKING_DIRECTORY);
			if (WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='\\' || WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='/')
				WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=0;

			char buff[128];
			MafiaNet::TimeUS time = MafiaNet::GetTimeUS();
#if defined(_WIN32)
			sprintf_s(buff, "%I64u", time);
#else
			sprintf_s(buff, "%llu", (long long unsigned int) time);
#endif

			char pathToPatch1[MAX_PATH], pathToPatch2[MAX_PATH];
			sprintf_s(pathToPatch1, "%s/patchClient_%s.tmp", WORKING_DIRECTORY, buff);
			FILE *fpPatch;
			if (fopen_s(&fpPatch, pathToPatch1, "wb")!=0)
				return PC_ERROR_PATCH_TARGET_MISSING;
			fwrite(patchContents, 1, patchSize, fpPatch);
			fclose(fpPatch);

			// Invoke xdelta
			// See https://code.google.com/p/xdelta/wiki/CommandLineSyntax
			char commandLine[512];
			_snprintf(commandLine, sizeof(commandLine)-1, "-d -f -s %s %s/patchClient_%s.tmp %s/newFile_%s.tmp", oldFilePath, WORKING_DIRECTORY, buff, WORKING_DIRECTORY, buff);
			commandLine[511]=0;
			
			SHELLEXECUTEINFOA shellExecuteInfo;
			shellExecuteInfo.cbSize = sizeof(SHELLEXECUTEINFO);
			shellExecuteInfo.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE;
			shellExecuteInfo.hwnd = nullptr;
			shellExecuteInfo.lpVerb = "open";
			shellExecuteInfo.lpFile = "xdelta3-3.0.6-win32.exe";
			shellExecuteInfo.lpParameters = commandLine;
			shellExecuteInfo.lpDirectory = nullptr;
			shellExecuteInfo.nShow = SW_SHOWNORMAL;
			shellExecuteInfo.hInstApp = nullptr;
			ShellExecuteExA(&shellExecuteInfo);

			// // ShellExecute is blocking, but if it writes a file to disk that file is not always immediately accessible after it returns. And this only happens in release, and only when not running in the debugger
			// ShellExecute(nullptr, "open", "xdelta3-3.0.6-win32.exe", commandLine, nullptr, SW_SHOWNORMAL);

			sprintf_s(pathToPatch2, "%s/newFile_%s.tmp", WORKING_DIRECTORY, buff);
			errno_t error = fopen_s(&fpPatch, pathToPatch2, "r+b");
			MafiaNet::TimeUS stopWaiting = time + 60000000;
			while (error!=0 && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				error = fopen_s(&fpPatch, pathToPatch2, "r+b");
			}
			if (error!=0)
			{
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nERROR: Could not open %s.\nerr=%i (%s)\narguments=%s\n", pathToPatch2, errno, buff2, commandLine);
				return PC_ERROR_PATCH_TARGET_MISSING;
			}

			fseek(fpPatch, 0, SEEK_END);
			*newFileSize = ftell(fpPatch);
			fseek(fpPatch, 0, SEEK_SET);
			*newFileContents = (char*) rakMalloc_Ex(*newFileSize, _FILE_AND_LINE_);
			fread(*newFileContents, 1, *newFileSize, fpPatch);
			fclose(fpPatch);

			int unlinkRes1 = _unlink(pathToPatch1);
			int unlinkRes2 = _unlink(pathToPatch2);
			while ((unlinkRes1!=0 || unlinkRes2!=0) && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				if (unlinkRes1!=0)
					unlinkRes1 = _unlink(pathToPatch1);
				if (unlinkRes2!=0)
					unlinkRes2 = _unlink(pathToPatch2);
			}

			if (unlinkRes1!=0) {
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nWARNING: unlink %s failed.\nerr=%i (%s)\n", pathToPatch1, errno, buff2);
			}
			if (unlinkRes2!=0) {
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nWARNING: unlink %s failed.\nerr=%i (%s)\n", pathToPatch2, errno, buff2);
			}

			return PC_WRITE_FILE;
		}
	}

} transferCallback;

int main(int argc, char **argv)
{
	if (argc<8)
	{
		printf("Arguments: serverIP, pathToGame, gameName, patchImmediately, localPort, serverPort, fullScan");
		return 0;
	}

	MafiaNet::SystemAddress TCPServerAddress= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	MafiaNet::AutopatcherClient autopatcherClient;
	MafiaNet::FileListTransfer fileListTransfer;
	MafiaNet::CloudClient cloudClient;
	autopatcherClient.SetFileListTransferPlugin(&fileListTransfer);
	bool didRebalance=false; // So we only reconnect to a lower load server once, for load balancing

	bool fullScan = argv[7][0]=='1';

	const int intLocalPort = atoi(argv[5]);
	if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
		return 2;
	}
	unsigned short localPort = static_cast<unsigned short>(intLocalPort);

	const int intServerPort = atoi(argv[6]);
	if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
		return 3;
	}
	unsigned short serverPort = static_cast<unsigned short>(intServerPort);

	MafiaNet::PacketizedTCP packetizedTCP;
	if (packetizedTCP.Start(localPort,1)==false)
	{
		printf("Failed to start TCP. Is the port already in use?");
		return 1;
	}
	packetizedTCP.AttachPlugin(&autopatcherClient);
	packetizedTCP.AttachPlugin(&fileListTransfer);

	MafiaNet::RakPeerInterface *rakPeer;
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::SocketDescriptor socketDescriptor(localPort,0);
	rakPeer->Startup(1,&socketDescriptor, 1);
	rakPeer->AttachPlugin(&cloudClient);
	DataStructures::List<MafiaNet::RakNetSocket2* > sockets;
	rakPeer->GetSockets(sockets);
	printf("Started on port %i\n", sockets[0]->GetBoundAddress().GetPort());


	char buff[512];
	strcpy_s(buff, argv[1]);

	rakPeer->Connect(buff, serverPort, 0, 0);

	printf("Connecting...\n");
	char appDir[512];
	strcpy_s(appDir, argv[2]);
	char appName[512];
	strcpy_s(appName, argv[3]);

	bool patchImmediately=argc>=5 && argv[4][0]=='1';

	MafiaNet::Packet *p;
	bool running = true;
	while(running)
	{
		MafiaNet::SystemAddress notificationAddress;
		notificationAddress=packetizedTCP.HasCompletedConnectionAttempt();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
			TCPServerAddress=notificationAddress;
		}
		notificationAddress=packetizedTCP.HasNewIncomingConnection();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_NEW_INCOMING_CONNECTION\n");
		notificationAddress=packetizedTCP.HasLostConnection();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_CONNECTION_LOST\n");
		notificationAddress=packetizedTCP.HasFailedConnectionAttempt();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			printf("ID_CONNECTION_ATTEMPT_FAILED TCP\n");
			break;
		}


		p=packetizedTCP.Receive();
		while (p)
		{
			if (p->data[0]==ID_AUTOPATCHER_REPOSITORY_FATAL_ERROR)
			{
				char buff2[256];
				MafiaNet::BitStream temp(p->data, p->length, false);
				temp.IgnoreBits(8);
				MafiaNet::StringCompressor::Instance()->DecodeString(buff2, 256, &temp);
				printf("ID_AUTOPATCHER_REPOSITORY_FATAL_ERROR\n");
				printf("%s\n", buff2);
				running = false;
				break;
			}
			else if (p->data[0]==ID_AUTOPATCHER_CANNOT_DOWNLOAD_ORIGINAL_UNMODIFIED_FILES)
			{
				printf("ID_AUTOPATCHER_CANNOT_DOWNLOAD_ORIGINAL_UNMODIFIED_FILES\n");
				running = false;
				break;
			}
			else if (p->data[0]==ID_AUTOPATCHER_FINISHED)
			{
				printf("ID_AUTOPATCHER_FINISHED with server time %f\n", autopatcherClient.GetServerDate());
				double srvDate=autopatcherClient.GetServerDate();
				FILE *fp;
				fopen_s(&fp, "srvDate", "wb");
				fwrite(&srvDate,sizeof(double),1,fp);
				fclose(fp);
				running = false;
				break;
			}
			else if (p->data[0]==ID_AUTOPATCHER_RESTART_APPLICATION)
			{
				printf("ID_AUTOPATCHER_RESTART_APPLICATION");
				running = false;
				break;
			}
			// Launch \"AutopatcherClientRestarter.exe autopatcherRestart.txt\"\nQuit this application immediately after to unlock files.\n");

			packetizedTCP.DeallocatePacket(p);
			p=packetizedTCP.Receive();
		}

		if (!running)
			break;

		p=rakPeer->Receive();
		while (p)
		{
			if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				// UploadInstanceToCloud(&cloudClient, p->guid);
				// GetClientSubscription(&cloudClient, p->guid);
				GetServers(&cloudClient, p->guid);
				break;
			}
			else if (p->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
			{
				printf("ID_CONNECTION_ATTEMPT_FAILED UDP\n");
				running = false;
				break;
			}
			else if (p->data[0]==ID_CLOUD_GET_RESPONSE)
			{
				MafiaNet::CloudQueryResult cloudQueryResult;
				cloudClient.OnGetReponse(&cloudQueryResult, p);
				unsigned int rowIndex;
				const bool wasCallToGetServers=cloudQueryResult.cloudQuery.keys[0].primaryKey=="CloudConnCount";
				printf("\n");
				if (wasCallToGetServers)
					printf("Downloaded server list. %i servers.\n", cloudQueryResult.rowsReturned.Size());

				unsigned short connectionsOnOurServer=65535;
				unsigned short lowestConnectionsServer=65535;
				MafiaNet::SystemAddress lowestConnectionAddress;

				for (rowIndex=0; rowIndex < cloudQueryResult.rowsReturned.Size(); rowIndex++)
				{
					MafiaNet::CloudQueryRow *row = cloudQueryResult.rowsReturned[rowIndex];
					if (wasCallToGetServers)
					{
						unsigned short connCount;
						MafiaNet::BitStream bsIn(row->data, row->length, false);
						bsIn.Read(connCount);
						printf("%i. Server found at %s with %i connections\n", rowIndex+1, row->serverSystemAddress.ToString(true), connCount);

						unsigned short connectionsExcludingOurselves;
						if (row->serverGUID==p->guid)
							connectionsExcludingOurselves=connCount-1;
						else
							connectionsExcludingOurselves=connCount;

						// Find the lowest load server (optional)
						if (p->guid==row->serverGUID)
						{
							connectionsOnOurServer=connectionsExcludingOurselves;
						}
						else if (connectionsExcludingOurselves < lowestConnectionsServer)
						{
							lowestConnectionsServer=connectionsExcludingOurselves;
							lowestConnectionAddress=row->serverSystemAddress;
						}
					}
				}


				// Do load balancing by reconnecting to lowest load server (optional)
				if (didRebalance==false && wasCallToGetServers)
				{
					if (cloudQueryResult.rowsReturned.Size()>0 && connectionsOnOurServer>lowestConnectionsServer)
					{
						printf("Reconnecting to lower load server %s\n", lowestConnectionAddress.ToString(false));

						rakPeer->CloseConnection(p->guid, true);
						// Wait for the thread to close, otherwise will immediately get back ID_CONNECTION_ATTEMPT_FAILED because no free outgoing connection slots
						// Alternatively, just call Startup() with 2 slots instead of 1
						RakSleep(500);

						rakPeer->Connect(lowestConnectionAddress.ToString(false), lowestConnectionAddress.GetPort(), 0, 0);

						// TCP Connect to new IP address
						packetizedTCP.Connect(lowestConnectionAddress.ToString(false),serverPort,false);
					}
					else
					{
						// TCP Connect to original IP address
						packetizedTCP.Connect(buff,serverPort,false);
					}

					didRebalance=true;
				}

				cloudClient.DeallocateWithDefaultAllocator(&cloudQueryResult);
			}

			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
		}

		if (!running)
			break;

		if (TCPServerAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS && patchImmediately==true)
		{
			patchImmediately=false;
			char restartFile[512];
			strcpy_s(restartFile, appDir);
			strcat_s(restartFile, "/autopatcherRestart.txt");

			double lastUpdateDate;

			if (fullScan==false)
			{
				FILE *fp;
				if (fopen_s(&fp, "srvDate", "rb") == 0)
				{
					fread(&lastUpdateDate, sizeof(lastUpdateDate), 1, fp);
					fclose(fp);
				}
				else
					lastUpdateDate=0;
			}
			else
				lastUpdateDate=0;

			if (autopatcherClient.PatchApplication(appName, appDir, lastUpdateDate, TCPServerAddress, &transferCallback, restartFile, argv[0]))
			{
				printf("Patching process starting.\n");
			}
			else
			{
				printf("Failed to start patching.\n");
				break;
			}
		}
		RakSleep(30);
	}

	// Dereference so the destructor doesn't crash
	autopatcherClient.SetFileListTransferPlugin(0);

	autopatcherClient.Clear();
	packetizedTCP.Stop();
	rakPeer->Shutdown(500,0);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
	return 0;
}
void UploadInstanceToCloud(MafiaNet::CloudClient *cloudClient, MafiaNet::RakNetGUID serverGuid)
{
	MafiaNet::CloudKey cloudKey(CLOUD_CLIENT_PRIMARY_KEY,0);
	MafiaNet::BitStream bs;
	bs.Write("Hello World"); // This could be anything such as player list, game name, etc.
	cloudClient->Post(&cloudKey, bs.GetData(), bs.GetNumberOfBytesUsed(), serverGuid);
}
void GetClientSubscription(MafiaNet::CloudClient *cloudClient, MafiaNet::RakNetGUID serverGuid)
{
	MafiaNet::CloudQuery cloudQuery;
	cloudQuery.keys.Push(MafiaNet::CloudKey(CLOUD_CLIENT_PRIMARY_KEY,0),_FILE_AND_LINE_);
	cloudQuery.subscribeToResults=false;
	cloudClient->Get(&cloudQuery, serverGuid);
}
void GetServers(MafiaNet::CloudClient *cloudClient, MafiaNet::RakNetGUID serverGuid)
{
	MafiaNet::CloudQuery cloudQuery;
	cloudQuery.keys.Push(MafiaNet::CloudKey("CloudConnCount",0),_FILE_AND_LINE_); // CloudConnCount is defined at the top of CloudServerHelper.cpp
	cloudQuery.subscribeToResults=false;
	cloudClient->Get(&cloudQuery, serverGuid);
}
